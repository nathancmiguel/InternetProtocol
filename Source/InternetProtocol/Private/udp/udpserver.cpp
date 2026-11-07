/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "udp/udpserver.h"
#include "net/acceptor.h"
#include "net/facade.h"

using IPNet::FFacadeAccess;

void UUDPServer::BeginDestroy() {
	FFacadeAccess::ReleaseUdp(Net);
	Super::BeginDestroy();
}

void UUDPServer::AddToRoot() {
	Super::AddToRoot();
}

void UUDPServer::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UUDPServer::IsRooted() {
	return Super::IsRooted();
}

bool UUDPServer::IsOpen() {
	return Net.bOpen;
}

FUdpEndpoint UUDPServer::LocalEndpoint() {
	return Net.Local;
}

FUdpEndpoint UUDPServer::RemoteEndpoint() {
	// Sender of the last datagram received.
	return Net.Remote;
}

void UUDPServer::SetRecvBufferSize(int Val) {
	recv_buffer_size = FMath::Clamp(Val, 1, 65536);
	if (Net.Socket) {
		Net.Socket->SetReceiveBufferSize(static_cast<size_t>(recv_buffer_size));
	}
}

int UUDPServer::GetRecvBufferSize() {
	return recv_buffer_size;
}

FErrorCode UUDPServer::GetErrorCode() {
	return Net.LastError;
}

bool UUDPServer::SendTo(const FString& Message, const FUdpEndpoint& Endpoint, const FDelegateUdpServerMessageSent& Callback) {
	asio::ip::udp::endpoint Target;
	if (!Net.Socket || !Net.bOpen || Message.IsEmpty() || !Endpoint.ToNative(Target)) {
		return false;
	}
	return Net.Socket->SendTo(IPNet::ToUtf8(Message), Target, IPNet::MakeSendCallback(this, Callback));
}

bool UUDPServer::SendBufferTo(const TArray<uint8>& Buffer, const FUdpEndpoint& Endpoint, const FDelegateUdpServerMessageSent& Callback) {
	asio::ip::udp::endpoint Target;
	if (!Net.Socket || !Net.bOpen || Buffer.Num() == 0 || !Endpoint.ToNative(Target)) {
		return false;
	}
	return Net.Socket->SendTo(IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), Target, IPNet::MakeSendCallback(this, Callback));
}

bool UUDPServer::Bind(const FServerBindOptions& BindOpts) {
	if (Net.Socket) {
		return false;
	}
	const uint64 Id = IPNet::NextSessionId();
	IPNet::FUdpEvents Events = FFacadeAccess::MakeUdpEvents(this, Id,
		[](UUDPServer* Self, const TArray<uint8>& Buffer, const asio::ip::udp::endpoint& From) {
			const FUdpEndpoint Sender(From);
			Self->Net.Remote = Sender;
			Self->OnMessage.Broadcast(Buffer, Buffer.Num(), Sender);
		});
	std::shared_ptr<IPNet::FUdpSocket> Socket = IPNet::FUdpSocket::Create(MoveTemp(Events), static_cast<size_t>(recv_buffer_size));
	if (!Socket) {
		FFacadeAccess::FailImmediately(this, IPNet::EError::NotRunning);
		return false;
	}

	asio::ip::address Address;
	asio::error_code Ec;
	if (!IPNet::MakeListenAddress(BindOpts.Address, BindOpts.Protocol, Address) || BindOpts.Port < 0 || BindOpts.Port > 65535) {
		Ec = IPNet::MakeError(IPNet::EError::InvalidArgument);
	} else {
		Socket->Bind(asio::ip::udp::endpoint(Address, static_cast<uint16>(BindOpts.Port)), BindOpts.bReuse_Address, Ec);
	}
	if (Ec) {
		Socket->Close();
		Net.LastError = FErrorCode(Ec);
		OnError.Broadcast(Net.LastError);
		return false;
	}

	Net.Socket = Socket;
	Net.SessionId = Id;
	Net.bOpen = true;
	Net.Local = FUdpEndpoint(Socket->BoundEndpoint());
	Net.Remote = FUdpEndpoint();
	Net.LastError = FErrorCode();
	Socket->Start();
	OnListening.Broadcast();
	return true;
}

void UUDPServer::Close() {
	FFacadeAccess::CloseUdp(Net);
}
