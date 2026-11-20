/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/transport.h"
#include "net/errors.h"
#include "net/tls.h"
#include <atomic>
#include <deque>
#include <type_traits>

namespace IPNet {
	using asio::ip::tcp;

	bool IsOrderlyClose(const asio::error_code& Error) {
		return Error == asio::error::eof || Error == asio::ssl::error::stream_truncated;
	}

	void FFlowControl::Consume(int64 Bytes) {
		const int64 Now = Pending.fetch_sub(Bytes, std::memory_order_acq_rel) - Bytes;
		if (Now < Limit / 2 && bPaused.exchange(false, std::memory_order_acq_rel)) {
			if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
				Strong->ResumeRead();
			}
		}
	}

	namespace {
		/** At most one warning per second per call site (a flood must not flood the log too). */
		bool ShouldLog(std::atomic<double>& Last) {
			const double Now = FPlatformTime::Seconds();
			double Previous = Last.load(std::memory_order_relaxed);
			return Now - Previous >= 1.0 && Last.compare_exchange_strong(Previous, Now);
		}
	}

	namespace {
		enum class EState : uint8 { Idle, Connecting, Handshaking, Open, Closing, Closed };

		constexpr std::chrono::seconds GracefulCloseTimeout{3};
		constexpr std::chrono::seconds FlushTimeout{5};
		constexpr size_t ReadChunkSize = 64 * 1024;

		template <bool bTls>
		class TStreamTransport final : public FTransport, public std::enable_shared_from_this<TStreamTransport<bTls>> {
			using FStream = std::conditional_t<bTls, asio::ssl::stream<tcp::socket>, tcp::socket>;

			struct FWriteItem {
				FBytes Data;
				FSendCallback Callback;
			};

		public:
			// Client constructor.
			TStreamTransport(std::shared_ptr<FIoCore> InCore, std::shared_ptr<const FTlsConfig> InTls,
			                 FTransportEvents InEvents, const FTransportOptions& InOptions)
				: Core(MoveTemp(InCore)),
				  Tls(MoveTemp(InTls)),
				  Executor(asio::make_strand(Core->Context)),
				  Stream(MakeClientStream(Executor, Tls)),
				  Resolver(Executor),
				  CloseTimer(Executor),
				  IdleTimer(Executor),
					  SetupTimer(Executor),
					  FlowTimer(Executor),
				  Events(MoveTemp(InEvents)),
				  Options(InOptions),
				  ReadBuffer(ReadChunkSize) {}

			// Accepted-socket constructor. The socket is already bound to its own strand.
			TStreamTransport(std::shared_ptr<FIoCore> InCore, tcp::socket&& Socket, std::shared_ptr<const FTlsConfig> InTls,
			                 FTransportEvents InEvents, const FTransportOptions& InOptions)
				: Core(MoveTemp(InCore)),
				  Tls(MoveTemp(InTls)),
				  Executor(Socket.get_executor()),
				  Stream(MakeAcceptedStream(MoveTemp(Socket), Tls)),
				  Resolver(Executor),
				  CloseTimer(Executor),
				  IdleTimer(Executor),
					  SetupTimer(Executor),
					  FlowTimer(Executor),
				  Events(MoveTemp(InEvents)),
				  Options(InOptions),
				  ReadBuffer(ReadChunkSize) {}

			virtual ~TStreamTransport() override {
				// Covers transports released without ever being started (Finish() already unregistered the others).
				Core->Unregister(this);
			}

			virtual void Connect(const std::string& Host, const std::string& Port, EProtocolType Protocol) override {
				asio::post(Executor, [Self = this->shared_from_this(), Host, Port, Protocol]() {
					Self->DoConnect(Host, Port, Protocol);
				});
			}

			virtual void StartAccepted() override {
				asio::post(Executor, [Self = this->shared_from_this()]() { Self->DoStartAccepted(); });
			}

			virtual bool Send(FBytes Data, FSendCallback Callback) override {
				const EState Current = State.load(std::memory_order_acquire);
				if (Current == EState::Closing || Current == EState::Closed) {
					return false;
				}
				const size_t Size = Data.size();
				if (QueuedBytes.fetch_add(Size, std::memory_order_acq_rel) + Size > Options.MaxQueuedBytes) {
					QueuedBytes.fetch_sub(Size, std::memory_order_acq_rel);
					static std::atomic<double> LastLog{0.0};
					if (ShouldLog(LastLog)) {
						UE_LOG(LogAsio, Warning, TEXT("InternetProtocol: send queue limit reached, message dropped."));
					}
					return false;
				}
				asio::post(Executor, [Self = this->shared_from_this(), Data = MoveTemp(Data), Callback = MoveTemp(Callback)]() mutable {
					Self->DoQueue(MoveTemp(Data), MoveTemp(Callback));
				});
				return true;
			}

			virtual void Close() override {
				if (Core->IsStopped()) {
					// The I/O threads are gone: nothing would ever run a posted handler. Nothing else runs
					// on this strand anymore either, so closing right here is safe.
					Finish();
					return;
				}
				asio::post(Executor, [Self = this->shared_from_this()]() { Self->DoClose(); });
			}

			virtual void abort() override {
				asio::post(Executor, [Self = this->shared_from_this()]() { Self->Finish(); });
			}

			virtual void CloseAfter(std::chrono::milliseconds Timeout) override {
				asio::post(Executor, [Self = this->shared_from_this(), Timeout]() {
					if (Self->State.load() == EState::Closed) {
						return;
					}
					Self->ArmCloseTimer(Timeout);
				});
			}

			virtual void Post(std::function<void()> Function) override {
				asio::post(Executor, [Self = this->shared_from_this(), Function = MoveTemp(Function)]() {
					if (Function) {
						Function();
					}
				});
			}

			virtual bool IsTls() const override { return bTls; }

			virtual void MarkEstablished() override {
				asio::post(Executor, [Self = this->shared_from_this()]() {
					Self->bEstablished = true;
					Self->SetupTimer.cancel();
				});
			}

			virtual void ResumeRead() override {
				asio::post(Executor, [Self = this->shared_from_this()]() {
					if (Self->bReadPaused && !Self->bReadPending && Self->State.load() == EState::Open) {
						Self->bReadPaused = false;
						Self->FlowTimer.cancel();
						Self->StartRead();
					}
				});
			}

		private:
			static FStream MakeClientStream(const asio::any_io_executor& Ex, const std::shared_ptr<const FTlsConfig>& InTls) {
				if constexpr (bTls) {
					return FStream(Ex, *InTls->Context);
				} else {
					return FStream(Ex);
				}
			}

			static FStream MakeAcceptedStream(tcp::socket&& Socket, const std::shared_ptr<const FTlsConfig>& InTls) {
				if constexpr (bTls) {
					return FStream(MoveTemp(Socket), *InTls->Context);
				} else {
					return FStream(MoveTemp(Socket));
				}
			}

			tcp::socket& Lowest() {
				if constexpr (bTls) {
					return Stream.next_layer();
				} else {
					return Stream;
				}
			}

			// ---- strand-only below ----

			void DoConnect(const std::string& Host, const std::string& Port, EProtocolType Protocol) {
				if (State.load() != EState::Idle) {
					return;
				}
				if (!Core->IsRunning()) {
					Fail(MakeError(EError::NotRunning));
					return;
				}
				if constexpr (bTls) {
					if (!Tls->IsValid()) {
						Fail(MakeError(EError::TlsSetupFailed));
						return;
					}
				}
				State.store(EState::Connecting);
				ConnectHost = Host;
				// "[::1]" style IPv6 literals are accepted (brackets are URL syntax, not part of the address).
				if (ConnectHost.size() > 2 && ConnectHost.front() == '[' && ConnectHost.back() == ']') {
					ConnectHost = ConnectHost.substr(1, ConnectHost.size() - 2);
				}
				ArmSetupTimer(Options.ConnectTimeout);
				auto OnResolvedHandler = [Self = this->shared_from_this()](const asio::error_code& Ec, const tcp::resolver::results_type& Results) {
					Self->OnResolved(Ec, Results);
				};
				if (Protocol == EProtocolType::Any) {
					// Every resolved address (IPv6 and IPv4) is tried in order by async_connect.
					Resolver.async_resolve(ConnectHost, Port, MoveTemp(OnResolvedHandler));
				} else {
					Resolver.async_resolve(Protocol == EProtocolType::V6 ? tcp::v6() : tcp::v4(), ConnectHost, Port, MoveTemp(OnResolvedHandler));
				}
			}

			void OnResolved(const asio::error_code& Ec, const tcp::resolver::results_type& Results) {
				if (State.load() != EState::Connecting) {
					return;
				}
				if (Ec) {
					Fail(Ec);
					return;
				}
				asio::async_connect(Lowest(), Results,
					[Self = this->shared_from_this()](const asio::error_code& ConnectEc, const tcp::endpoint&) {
						Self->OnConnected(ConnectEc);
					});
			}

			void OnConnected(const asio::error_code& Ec) {
				if (State.load() != EState::Connecting) {
					return;
				}
				if (Ec) {
					Fail(Ec);
					return;
				}
				if constexpr (bTls) {
					asio::error_code ConfigEc;
					if (!Tls->ConfigureClientStream(Stream, ConnectHost, ConfigEc)) {
						Fail(ConfigEc ? ConfigEc : MakeError(EError::TlsSetupFailed));
						return;
					}
					State.store(EState::Handshaking);
					Stream.async_handshake(asio::ssl::stream_base::client,
						[Self = this->shared_from_this()](const asio::error_code& HandshakeEc) {
							Self->OnHandshake(HandshakeEc);
						});
				} else {
					OnOpen();
				}
			}

			void DoStartAccepted() {
				if (State.load() != EState::Idle) {
					return;
				}
				if constexpr (bTls) {
					if (!Tls->IsValid()) {
						Fail(MakeError(EError::TlsSetupFailed));
						return;
					}
					State.store(EState::Handshaking);
					ArmSetupTimer(Options.HandshakeTimeout);
					ArmIdle();
					Stream.async_handshake(asio::ssl::stream_base::server,
						[Self = this->shared_from_this()](const asio::error_code& HandshakeEc) {
							Self->OnHandshake(HandshakeEc);
						});
				} else {
					OnOpen();
				}
			}

			void OnHandshake(const asio::error_code& Ec) {
				if (State.load() != EState::Handshaking) {
					return;
				}
				if (Ec) {
					Fail(Ec);
					return;
				}
				bHandshakeDone = true;
				OnOpen();
			}

			void OnOpen() {
				State.store(EState::Open);
				if (Options.bEstablishOnOpen) {
					bEstablished = true;
					SetupTimer.cancel();
				} else if (!bEstablished && !bSetupTimerArmed) {
					// Plain accepted connection: the protocol handshake starts now.
					ArmSetupTimer(Options.HandshakeTimeout);
				}
				asio::error_code Ec;
				const tcp::endpoint Local = Lowest().local_endpoint(Ec);
				const tcp::endpoint Remote = Lowest().remote_endpoint(Ec);
				ArmIdle();
				if (Events.OnConnected) {
					Events.OnConnected(Local, Remote);
				}
				// The owner may have closed us from inside OnConnected.
				if (State.load() != EState::Open) {
					return;
				}
				StartRead();
				StartWrite();
			}

			void StartRead() {
				bReadPending = true;
				Stream.async_read_some(asio::buffer(ReadBuffer),
					[Self = this->shared_from_this()](const asio::error_code& Ec, size_t Bytes) {
						Self->OnRead(Ec, Bytes);
					});
			}

			void OnRead(const asio::error_code& Ec, size_t Bytes) {
				bReadPending = false;
				const EState Current = State.load();
				if (Current == EState::Closed) {
					return;
				}
				if (Current == EState::Closing) {
					// Graceful close: the read was cancelled (or ended) so the TLS shutdown can start.
					if (bShutdownAfterRead) {
						bShutdownAfterRead = false;
						StartTlsShutdown();
						return;
					}
					if constexpr (!bTls) {
						if (bHalfClosed) {
							// Half-close: our FIN is sent; drain (and discard) until the peer's EOF, bounded by the
							// close timer. EOF or any error ends the connection.
							if (Ec || Bytes == 0) {
								Finish();
							} else {
								StartRead();
							}
						}
					}
					return;
				}
				if (Ec) {
					if (IsOrderlyClose(Ec)) {
						Finish();
					} else {
						Fail(Ec);
					}
					return;
				}
				ArmIdle();
				if (Events.OnData && Bytes > 0) {
					Events.OnData(ReadBuffer.data(), Bytes);
				}
				if (State.load() != EState::Open) {
					return;
				}
				if (Options.Flow && Options.Flow->IsOverLimit()) {
					// The Game Thread is behind: stop reading. Set the flag first, then re-check, so a Consume()
					// racing with us cannot be missed.
					Options.Flow->bPaused.store(true, std::memory_order_release);
					if (Options.Flow->IsOverLimit() || !Options.Flow->bPaused.exchange(false, std::memory_order_acq_rel)) {
						bReadPaused = true;
						ArmFlowRecheck();
						return;
					}
				}
				StartRead();
			}

			void DoQueue(FBytes&& Data, FSendCallback&& Callback) {
				const EState Current = State.load();
				if (Current == EState::Closed || (Current == EState::Closing && !bFlushing)) {
					QueuedBytes.fetch_sub(Data.size(), std::memory_order_acq_rel);
					if (Callback) {
						Callback(asio::error::operation_aborted, 0);
					}
					return;
				}
				WriteQueue.push_back(FWriteItem{MoveTemp(Data), MoveTemp(Callback)});
				StartWrite();
			}

			void StartWrite() {
				const EState Current = State.load();
				if (bWriting || WriteQueue.empty() || (Current != EState::Open && Current != EState::Closing)) {
					return;
				}
				bWriting = true;
				asio::async_write(Stream, asio::buffer(WriteQueue.front().Data),
					[Self = this->shared_from_this()](const asio::error_code& Ec, size_t Bytes) {
						Self->OnWrite(Ec, Bytes);
					});
			}

			void OnWrite(const asio::error_code& Ec, size_t Bytes) {
				bWriting = false;
				if (WriteQueue.empty()) {
					return;
				}
				FWriteItem Item = MoveTemp(WriteQueue.front());
				WriteQueue.pop_front();
				QueuedBytes.fetch_sub(Item.Data.size(), std::memory_order_acq_rel);
				if (Item.Callback) {
					Item.Callback(Ec, Bytes);
				}
				if (State.load() == EState::Closed) {
					return;
				}
				if (Ec) {
					Fail(Ec);
					return;
				}
				ArmIdle();
				if (!WriteQueue.empty()) {
					StartWrite();
				} else if (State.load() == EState::Closing && bFlushing) {
					bFlushing = false;
					BeginShutdown();
				}
			}

			void DoClose() {
				const EState Current = State.load();
				if (Current == EState::Closing || Current == EState::Closed) {
					return;
				}
				if (Current != EState::Open) {
					// Not connected yet: nothing to flush or shut down.
					Finish();
					return;
				}
				State.store(EState::Closing);
				IdleTimer.cancel();
				if (bWriting || !WriteQueue.empty()) {
					bFlushing = true;
					ArmCloseTimer(FlushTimeout);
					return;
				}
				BeginShutdown();
			}

			void BeginShutdown() {
				ArmCloseTimer(GracefulCloseTimeout);
				if constexpr (bTls) {
					if (bHandshakeDone) {
						if (bReadPending) {
							// ssl::stream does not allow a shutdown concurrently with a read: cancel the read first,
							// the shutdown starts from its completion handler.
							bShutdownAfterRead = true;
							asio::error_code Ignored;
							Lowest().cancel(Ignored);
							return;
						}
						StartTlsShutdown();
						return;
					}
				}
				asio::error_code Ignored;
				if (bReadPending) {
					// Graceful TCP close: send FIN, keep reading until the peer closes too (bounded by the
					// close timer). Data still in flight from the peer is not turned into a connection reset.
					Lowest().shutdown(tcp::socket::shutdown_send, Ignored);
					bHalfClosed = true;
					return;
				}
				Lowest().shutdown(tcp::socket::shutdown_both, Ignored);
				Finish();
			}

			void StartTlsShutdown() {
				if constexpr (bTls) {
					Stream.async_shutdown([Self = this->shared_from_this()](const asio::error_code&) {
						// Any outcome (including eof / stream_truncated / timeout cancel) ends the connection.
						Self->Finish();
					});
				}
			}

			void ArmCloseTimer(std::chrono::milliseconds Timeout) {
				CloseTimer.expires_after(Timeout);
				CloseTimer.async_wait([Self = this->shared_from_this()](const asio::error_code& Ec) {
					if (!Ec) {
						Self->Finish();
					}
				});
			}

			void ArmIdle() {
				if (Options.IdleTimeout.count() <= 0) {
					return;
				}
				IdleTimer.expires_after(Options.IdleTimeout);
				IdleTimer.async_wait([Self = this->shared_from_this()](const asio::error_code& Ec) {
					if (!Ec) {
						if (Self->State.load() == EState::Open && Self->Events.OnError) {
							Self->Events.OnError(MakeError(EError::TimedOut));
						}
						Self->DoClose();
					}
				});
			}

			/**
			 * Safety net while reading is paused: Consume() normally resumes us, but a missed wake-up must
			 * never stall a connection forever, so the limit is re-checked periodically too.
			 */
			void ArmFlowRecheck() {
				FlowTimer.expires_after(std::chrono::milliseconds(100));
				FlowTimer.async_wait([Self = this->shared_from_this()](const asio::error_code& Ec) {
					if (Ec || !Self->bReadPaused || Self->State.load() != EState::Open) {
						return;
					}
					if (Self->Options.Flow && Self->Options.Flow->IsOverLimit()) {
						Self->ArmFlowRecheck();
						return;
					}
					Self->bReadPaused = false;
					if (Self->Options.Flow) {
						Self->Options.Flow->bPaused.store(false, std::memory_order_release);
					}
					if (!Self->bReadPending) {
						Self->StartRead();
					}
				});
			}

			/** Connect (client) or handshake (accepted) deadline. */
			void ArmSetupTimer(std::chrono::milliseconds Timeout) {
				if (Timeout.count() <= 0 || bEstablished) {
					return;
				}
				bSetupTimerArmed = true;
				SetupTimer.expires_after(Timeout);
				SetupTimer.async_wait([Self = this->shared_from_this()](const asio::error_code& Ec) {
					if (!Ec && !Self->bEstablished && Self->State.load() != EState::Closed) {
						Self->Fail(MakeError(EError::TimedOut));
					}
				});
			}

			void Fail(const asio::error_code& Ec) {
				if (State.load() == EState::Closed) {
					return;
				}
				if (Ec && Ec != asio::error::operation_aborted && Events.OnError) {
					Events.OnError(Ec);
				}
				Finish();
			}

			void Finish() {
				if (State.exchange(EState::Closed) == EState::Closed) {
					return;
				}
				asio::error_code Ignored;
				Resolver.cancel();
				CloseTimer.cancel();
				IdleTimer.cancel();
				SetupTimer.cancel();
				FlowTimer.cancel();
				Lowest().close(Ignored);

				std::deque<FWriteItem> Dropped = MoveTemp(WriteQueue);
				WriteQueue.clear();
				for (FWriteItem& Item : Dropped) {
					QueuedBytes.fetch_sub(Item.Data.size(), std::memory_order_acq_rel);
					if (Item.Callback) {
						Item.Callback(asio::error::operation_aborted, 0);
					}
				}

				Core->Unregister(this);
				FTransportEvents Released = MoveTemp(Events);
				Events = FTransportEvents();
				if (Released.OnClosed) {
					Released.OnClosed();
				}
			}

			std::shared_ptr<FIoCore> Core;
			std::shared_ptr<const FTlsConfig> Tls;
			asio::any_io_executor Executor;
			FStream Stream;
			tcp::resolver Resolver;
			asio::steady_timer CloseTimer;
			asio::steady_timer IdleTimer;
			asio::steady_timer SetupTimer;
			asio::steady_timer FlowTimer;
			FTransportEvents Events;
			FTransportOptions Options;
			std::vector<uint8> ReadBuffer;
			std::deque<FWriteItem> WriteQueue;
			std::string ConnectHost;
			std::atomic<EState> State{EState::Idle};
			std::atomic<size_t> QueuedBytes{0};
			bool bWriting = false;
			bool bReadPending = false;
			bool bFlushing = false;
			bool bShutdownAfterRead = false;
			bool bHandshakeDone = false;
			bool bEstablished = false;
			bool bSetupTimerArmed = false;
			bool bReadPaused = false;
			bool bHalfClosed = false;
		};

		template <typename T>
		std::shared_ptr<FTransport> Register(const std::shared_ptr<FIoCore>& Core, std::shared_ptr<T> Transport,
		                                     const FTransportOptions& Options) {
			Core->Register(Transport);
			if (Options.Flow) {
				Options.Flow->Transport = Transport;
			}
			return Transport;
		}
	}

	std::shared_ptr<FTransport> MakeTcpClient(FTransportEvents Events, const FTransportOptions& Options) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core) {
			return nullptr;
		}
		return Register(Core, std::make_shared<TStreamTransport<false>>(Core, nullptr, MoveTemp(Events), Options), Options);
	}

	std::shared_ptr<FTransport> MakeTlsClient(std::shared_ptr<const FTlsConfig> Tls, FTransportEvents Events,
	                                          const FTransportOptions& Options) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core || !Tls || !Tls->Context) {
			return nullptr;
		}
		return Register(Core, std::make_shared<TStreamTransport<true>>(Core, MoveTemp(Tls), MoveTemp(Events), Options), Options);
	}

	std::shared_ptr<FTransport> MakeTcpAccepted(tcp::socket&& Socket, FTransportEvents Events, const FTransportOptions& Options) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core) {
			return nullptr;
		}
		return Register(Core, std::make_shared<TStreamTransport<false>>(Core, MoveTemp(Socket), nullptr, MoveTemp(Events), Options), Options);
	}

	std::shared_ptr<FTransport> MakeTlsAccepted(tcp::socket&& Socket, std::shared_ptr<const FTlsConfig> Tls,
	                                            FTransportEvents Events, const FTransportOptions& Options) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core || !Tls || !Tls->Context) {
			return nullptr;
		}
		return Register(Core, std::make_shared<TStreamTransport<true>>(Core, MoveTemp(Socket), MoveTemp(Tls), MoveTemp(Events), Options), Options);
	}
}
