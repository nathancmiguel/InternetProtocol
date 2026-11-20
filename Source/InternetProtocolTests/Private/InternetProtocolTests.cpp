/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPTestListener.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/UObjectGlobals.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
THIRD_PARTY_INCLUDES_END
#undef UI

IMPLEMENT_MODULE(FDefaultModuleImpl, InternetProtocolTests)

#if WITH_DEV_AUTOMATION_TESTS

namespace IPTests {
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Latent wait: polls Predicate every frame (Game Thread tasks run in between) until true or timeout. */
	void WaitUntil(FAutomationTestBase* Test, const FString& What, TFunction<bool()> Predicate, double TimeoutSeconds = 10.0) {
		TSharedRef<double> Start = MakeShared<double>(-1.0);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, What, Predicate, TimeoutSeconds, Start]() {
			if (*Start < 0.0) {
				*Start = FPlatformTime::Seconds();
			}
			if (Predicate()) {
				return true;
			}
			if (FPlatformTime::Seconds() - *Start > TimeoutSeconds) {
				Test->AddError(FString::Printf(TEXT("Timed out waiting for: %s"), *What));
				return true;
			}
			return false;
		}));
	}

	void Then(TFunction<void()> Step) {
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() {
			Step();
			return true;
		}));
	}

	template <typename T>
	T* Make() {
		T* Object = NewObject<T>();
		Object->AddToRoot();
		return Object;
	}

	template <typename T>
	void Release(T* Object) {
		if (IsValid(Object)) {
			Object->RemoveFromRoot();
		}
	}

	TArray<uint8> Pattern(int32 Size) {
		TArray<uint8> Data;
		Data.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index) {
			Data[Index] = static_cast<uint8>((Index * 31 + 7) & 0xFF);
		}
		return Data;
	}

	FServerBindOptions Loopback() {
		FServerBindOptions Options;
		Options.Address = TEXT("127.0.0.1");
		Options.Port = 0; // ephemeral
		return Options;
	}

	FClientBindOptions ClientTo(int32 Port, const FString& Address = TEXT("127.0.0.1")) {
		FClientBindOptions Options;
		Options.Address = Address;
		Options.Port = FString::FromInt(Port);
		return Options;
	}

	/** Self-signed certificate for "localhost" / 127.0.0.1 (EC P-256, SHA-256). */
	bool MakeSelfSignedCertificate(FString& OutCertPem, FString& OutKeyPem) {
		EVP_PKEY_CTX* KeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		EVP_PKEY* Key = nullptr;
		bool bOk = KeyCtx && EVP_PKEY_keygen_init(KeyCtx) == 1 &&
		           EVP_PKEY_CTX_set_ec_paramgen_curve_nid(KeyCtx, NID_X9_62_prime256v1) == 1 &&
		           EVP_PKEY_keygen(KeyCtx, &Key) == 1;
		EVP_PKEY_CTX_free(KeyCtx);
		X509* Cert = bOk ? X509_new() : nullptr;
		if (Cert) {
			X509_set_version(Cert, 2);
			ASN1_INTEGER_set(X509_get_serialNumber(Cert), 1);
			X509_gmtime_adj(X509_getm_notBefore(Cert), -3600);
			X509_gmtime_adj(X509_getm_notAfter(Cert), 3600 * 24);
			X509_set_pubkey(Cert, Key);
			X509_NAME* Name = X509_get_subject_name(Cert);
			X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
			X509_set_issuer_name(Cert, Name);
			X509V3_CTX V3;
			X509V3_set_ctx_nodb(&V3);
			X509V3_set_ctx(&V3, Cert, Cert, nullptr, nullptr, 0);
			if (X509_EXTENSION* San = X509V3_EXT_conf_nid(nullptr, &V3, NID_subject_alt_name, const_cast<char*>("DNS:localhost,IP:127.0.0.1"))) {
				X509_add_ext(Cert, San, -1);
				X509_EXTENSION_free(San);
			}
			bOk = X509_sign(Cert, Key, EVP_sha256()) > 0;
		}
		if (bOk) {
			BIO* CertBio = BIO_new(BIO_s_mem());
			BIO* KeyBio = BIO_new(BIO_s_mem());
			PEM_write_bio_X509(CertBio, Cert);
			PEM_write_bio_PrivateKey(KeyBio, Key, nullptr, nullptr, 0, nullptr, nullptr);
			char* Data = nullptr;
			long Len = BIO_get_mem_data(CertBio, &Data);
			OutCertPem = FString(static_cast<int32>(Len), ANSI_TO_TCHAR(Data));
			Len = BIO_get_mem_data(KeyBio, &Data);
			OutKeyPem = FString(static_cast<int32>(Len), ANSI_TO_TCHAR(Data));
			BIO_free(CertBio);
			BIO_free(KeyBio);
		}
		X509_free(Cert);
		EVP_PKEY_free(Key);
		return bOk;
	}
}

using namespace IPTests;

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPTcpEchoTest, "InternetProtocol.TCP.EchoLargeBuffer", IPTests::Flags)
bool FIPTcpEchoTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	UTCPClient* Client = Make<UTCPClient>();
	Server->OnClientAccepted.AddDynamic(L, &UIPTestListener::OnTcpAccepted);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnBytes);
	Client->OnError.AddDynamic(L, &UIPTestListener::OnError);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnClose);

	if (!TestTrue(TEXT("Server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;
	TestTrue(TEXT("Ephemeral port assigned"), Port > 0);
	TestTrue(TEXT("Client connect starts"), Client->Connect(ClientTo(Port)));
	TestFalse(TEXT("Second Connect while connecting is rejected"), Client->Connect(ClientTo(Port)));

	const TArray<uint8> Payload = Pattern(1024 * 1024);
	WaitUntil(this, TEXT("client connected"), [L]() { return L->Connected == 1; });
	Then([this, Client, Payload]() {
		TestTrue(TEXT("IsOpen after OnConnected"), Client->IsOpen());
		FDelegateTcpClientMessageSent None;
		// Many concurrent writes: they must be queued, never interleaved.
		const int32 Chunk = 64 * 1024;
		for (int32 Offset = 0; Offset < Payload.Num(); Offset += Chunk) {
			TArray<uint8> Part(Payload.GetData() + Offset, FMath::Min(Chunk, Payload.Num() - Offset));
			TestTrue(TEXT("WriteBuffer accepted"), Client->WriteBuffer(Part, None));
		}
	});
	WaitUntil(this, TEXT("1 MiB echoed"), [L, Payload]() { return L->Received.Num() >= Payload.Num(); });
	Then([this, L, Payload, Client, Server]() {
		TestTrue(TEXT("Echoed bytes identical and in order"), L->Received == Payload);
		TestEqual(TEXT("No client error"), L->Errors, 0);
		Client->Close();
		Server->Close();
	});
	WaitUntil(this, TEXT("client OnClose"), [L]() { return L->Closed == 1; });
	Then([this, L, Client, Server]() {
		TestFalse(TEXT("Closed client is not open"), Client->IsOpen());
		TestEqual(TEXT("OnClose delivered exactly once"), L->Closed, 1);
		Release(Client);
		Release(Server);
		Release(L);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPTcpManyClientsTest, "InternetProtocol.TCP.ManyConcurrentClients", IPTests::Flags)
bool FIPTcpManyClientsTest::RunTest(const FString& Parameters) {
	// More connections than I/O threads (and than CPU cores): the old one-thread-per-connection model starved here.
	constexpr int32 NumClients = 128;
	UIPTestListener* ServerListener = Make<UIPTestListener>();
	UIPTestListener* ClientListener = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	Server->OnClientAccepted.AddDynamic(ServerListener, &UIPTestListener::OnTcpAccepted);
	if (!TestTrue(TEXT("Server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;

	TSharedRef<TArray<UTCPClient*>> Clients = MakeShared<TArray<UTCPClient*>>();
	for (int32 Index = 0; Index < NumClients; ++Index) {
		UTCPClient* Client = Make<UTCPClient>();
		Client->OnConnected.AddDynamic(ClientListener, &UIPTestListener::OnConnected);
		Client->OnMessage.AddDynamic(ClientListener, &UIPTestListener::OnBytes);
		Client->OnError.AddDynamic(ClientListener, &UIPTestListener::OnError);
		Client->Connect(ClientTo(Port));
		Clients->Add(Client);
	}
	WaitUntil(this, TEXT("all clients connected"), [ClientListener]() { return ClientListener->Connected == NumClients; }, 20.0);
	Then([Clients]() {
		for (UTCPClient* Client : *Clients) {
			Client->Write(TEXT("ping"), FDelegateTcpClientMessageSent());
		}
	});
	WaitUntil(this, TEXT("all echoes received"), [ClientListener]() { return ClientListener->Received.Num() == NumClients * 4; }, 20.0);
	Then([this, Clients, Server, ServerListener, ClientListener]() {
		TestEqual(TEXT("Server accepted every client"), ServerListener->Accepted, NumClients);
		TestEqual(TEXT("Server tracks every client"), Server->Clients().Num(), NumClients);
		TestEqual(TEXT("No client errors"), ClientListener->Errors, 0);
		if (ClientListener->Errors > 0) {
			AddError(FString::Printf(TEXT("Last client error: %d %s"), ClientListener->LastError.Value, *ClientListener->LastError.Message));
		}
		Server->Close();
		for (UTCPClient* Client : *Clients) {
			Release(Client);
		}
		Release(Server);
		Release(ServerListener);
		Release(ClientListener);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPDestroyWhileBusyTest, "InternetProtocol.Lifecycle.DestroyWhileBusy", IPTests::Flags)
bool FIPDestroyWhileBusyTest::RunTest(const FString& Parameters) {
	// Objects garbage-collected while connecting / transferring must not crash or deliver events afterwards.
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	Server->OnClientAccepted.AddDynamic(L, &UIPTestListener::OnTcpAccepted);
	if (!TestTrue(TEXT("Server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;
	const TArray<uint8> Payload = Pattern(4 * 1024 * 1024);

	for (int32 Index = 0; Index < 16; ++Index) {
		UTCPClient* Client = NewObject<UTCPClient>(); // not rooted: collectable
		Client->OnMessage.AddDynamic(L, &UIPTestListener::OnBytes);
		Client->Connect(ClientTo(Port));
		UHttpClient* Http = NewObject<UHttpClient>();
		FClientBindOptions Bind = ClientTo(Port);
		Http->SetHost(Bind);
		Http->Request(FHttpRequest(), FDelegateHttpClientResponse());
		UWSClient* Ws = NewObject<UWSClient>();
		Ws->Connect(ClientTo(Port));
	}
	Then([]() { CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); });

	TSharedRef<UTCPClient*> Busy = MakeShared<UTCPClient*>(nullptr);
	Then([Busy, Port, L]() {
		*Busy = NewObject<UTCPClient>();
		(*Busy)->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
		(*Busy)->Connect(ClientTo(Port));
	});
	WaitUntil(this, TEXT("busy client connected"), [L]() { return L->Connected >= 1; });
	Then([Busy, Payload]() {
		(*Busy)->WriteBuffer(Payload, FDelegateTcpClientMessageSent());
		(*Busy)->WriteBuffer(Payload, FDelegateTcpClientMessageSent());
		*Busy = nullptr;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); // destroyed with megabytes still queued
	});
	TSharedRef<double> Until = MakeShared<double>(0.0);
	Then([Until]() { *Until = FPlatformTime::Seconds() + 1.0; });
	WaitUntil(this, TEXT("settle"), [Until]() { return FPlatformTime::Seconds() > *Until; });
	Then([this, Server, L]() {
		TestTrue(TEXT("Still alive after destroying busy objects"), true);
		Server->Close();
		Release(Server);
		Release(L);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPUdpEchoTest, "InternetProtocol.UDP.Echo", IPTests::Flags)
bool FIPUdpEchoTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UUDPServer* Server = Make<UUDPServer>();
	UUDPClient* Client = Make<UUDPClient>();
	ServerL->UdpServer = Server;
	Server->OnMessage.AddDynamic(ServerL, &UIPTestListener::OnUdpServerMessage);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnBytes);
	if (!TestTrue(TEXT("UDP server binds"), Server->Bind(Loopback()))) {
		return false;
	}
	Client->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("udp client connected"), [L]() { return L->Connected == 1; });
	Then([Client]() {
		Client->Send(TEXT("olá ✓"), FDelegateUdpClientMessageSent());
	});
	WaitUntil(this, TEXT("udp echo"), [L]() { return L->Messages >= 1; });
	Then([this, L, ServerL, Client, Server]() {
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(L->Received.GetData()), L->Received.Num());
		TestEqual(TEXT("UTF-8 datagram round trip"), FString(Text.Length(), Text.Get()), FString(TEXT("olá ✓")));
		TestEqual(TEXT("Server saw the client endpoint"), ServerL->LastUdpSender.Port, Client->LocalEndpoint().Port);
		Client->Close();
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpTest, "InternetProtocol.HTTP.RequestsAndRouting", IPTests::Flags)
bool FIPHttpTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UHttpServer* Server = Make<UHttpServer>();
	UHttpClient* Client = Make<UHttpClient>();
	FDelegateHttpServerRequest Hello;
	Hello.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpHello));
	FDelegateHttpServerRequest Echo;
	Echo.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpEcho));
	Server->Get(TEXT("/hello"), Hello);
	Server->Post(TEXT("/echo"), Echo);
	if (!TestTrue(TEXT("HTTP server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Client->SetHost(ClientTo(Server->LocalEndpoint().Port));

	FDelegateHttpClientResponse OnResponse;
	OnResponse.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpResponse));

	FHttpRequest Get;
	Get.Path = TEXT("/hello");
	Get.Params.Add(TEXT("name"), TEXT("José & Co"));
	FHttpRequest Post;
	Post.Method = ERequestMethod::POST;
	Post.Path = TEXT("/echo");
	Post.Body = FString::ChrN(200 * 1024, TEXT('x')) + TEXT("ção");
	FHttpRequest Missing;
	Missing.Path = TEXT("/missing");

	// Queued back to back on one keep-alive connection.
	Client->Request(Get, OnResponse);
	Client->Request(Post, OnResponse);
	Client->Request(Missing, OnResponse);

	WaitUntil(this, TEXT("3 HTTP responses"), [L]() { return L->Responses.Num() == 3; });
	Then([this, L, Post, Client, Server]() {
		for (const FErrorCode& Error : L->ResponseErrors) {
			TestEqual(TEXT("No transport error"), Error.Value, 0);
		}
		TestEqual(TEXT("GET status"), L->Responses[0].Status_Code, 200);
		TestEqual(TEXT("Query params URL-decoded"), L->Responses[0].Body, FString(TEXT("hello José & Co")));
		TestEqual(TEXT("POST status"), L->Responses[1].Status_Code, 201);
		TestTrue(TEXT("POST body echoed byte-exact (UTF-8 Content-Length)"), L->Responses[1].Body == Post.Body);
		TestEqual(TEXT("Unrouted path answers 404"), L->Responses[2].Status_Code, 404);
		Client->Close();
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpConnectFailTest, "InternetProtocol.HTTP.ConnectionRefused", IPTests::Flags)
bool FIPHttpConnectFailTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UHttpClient* Client = Make<UHttpClient>();
	// Find a free port and close it again so nothing listens there.
	UTCPServer* Probe = Make<UTCPServer>();
	Probe->Open(Loopback());
	const int32 Port = Probe->LocalEndpoint().Port;
	Probe->Close();
	Client->SetHost(ClientTo(Port));
	FDelegateHttpClientResponse OnResponse;
	OnResponse.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpResponse));
	Client->Request(FHttpRequest(), OnResponse);
	Client->Request(FHttpRequest(), OnResponse);
	WaitUntil(this, TEXT("both requests fail"), [L]() { return L->Responses.Num() == 2; });
	Then([this, L, Client, Probe]() {
		TestTrue(TEXT("First request reports an error"), L->ResponseErrors[0].Value != 0);
		TestTrue(TEXT("Queued request also fails (no infinite retry)"), L->ResponseErrors[1].Value != 0);
		Release(Client);
		Release(Probe);
		Release(L);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPWebSocketTest, "InternetProtocol.WebSocket.EchoAndClose", IPTests::Flags)
bool FIPWebSocketTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UWSServer* Server = Make<UWSServer>();
	UWSClient* Client = Make<UWSClient>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnWsAccepted);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnWsConnected);
	Client->OnUnexpectedHandshake.AddDynamic(L, &UIPTestListener::OnWsUnexpected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnWsMessage);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnWsClose);
	Client->OnError.AddDynamic(L, &UIPTestListener::OnError);
	if (!TestTrue(TEXT("WS server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Client->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("ws handshake"), [L]() { return L->Connected == 1; });
	const FString Text = TEXT("olá mundo ✓ \U0001F600");
	Then([Client, Text]() {
		Client->Write(Text, FDataframe(), FDelegateWsClientMessageSent());
	});
	WaitUntil(this, TEXT("text echo"), [L]() { return L->Messages == 1; });
	const TArray<uint8> Big = Pattern(1024 * 1024 + 3);
	Then([this, L, Client, Text, Big]() {
		TestFalse(TEXT("Text frame flagged as text"), L->bLastBinary);
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(L->Received.GetData()), L->Received.Num());
		TestEqual(TEXT("Non-ASCII text round trip"), FString(Decoded.Length(), Decoded.Get()), Text);
		// > 64 KiB: 64-bit length header, split across many TCP reads on the receiving side.
		Client->WriteBuffer(Big, FDataframe(), FDelegateWsClientMessageSent());
	});
	WaitUntil(this, TEXT("1 MiB binary echo"), [L]() { return L->Messages == 2; });
	Then([this, L, Client, Big]() {
		TestTrue(TEXT("Binary frame flagged as binary"), L->bLastBinary);
		TestTrue(TEXT("1 MiB binary echoed intact"), L->Received == Big);
		Client->End(1000, TEXT("bye"));
		TestFalse(TEXT("Not open after End"), Client->IsOpen());
		TestFalse(TEXT("Write refused after End"), Client->Write(TEXT("late"), FDataframe(), FDelegateWsClientMessageSent()));
	});
	WaitUntil(this, TEXT("ws close handshake"), [L]() { return L->Closed == 1; });
	Then([this, L, ServerL, Client, Server]() {
		TestEqual(TEXT("Close code"), L->LastCloseCode, 1000);
		TestEqual(TEXT("No unexpected handshake"), L->Errors, 0);
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPTlsTest, "InternetProtocol.TLS.VerificationAndEcho", IPTests::Flags)
bool FIPTlsTest::RunTest(const FString& Parameters) {
	FString CertPem, KeyPem;
	if (!TestTrue(TEXT("Self-signed certificate generated"), MakeSelfSignedCertificate(CertPem, KeyPem))) {
		return false;
	}
	FSecurityContextOpts ServerOpts;
	ServerOpts.Cert = CertPem;
	ServerOpts.Private_Key = KeyPem;
	ServerOpts.Verify_Mode = EVerifyMode::None; // do not request client certificates

	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServerSsl* Server = Make<UTCPServerSsl>();
	Server->Construct(ServerOpts);
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnTcpSslAccepted);
	if (!TestTrue(TEXT("TLS server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;

	// 1) Trusted CA + matching host name: handshake succeeds, data flows.
	FSecurityContextOpts Trusted;
	Trusted.CA_Cert = CertPem;
	Trusted.bUseSystemRootCertificates = false;
	UIPTestListener* Good = Make<UIPTestListener>();
	UTCPClientSsl* GoodClient = Make<UTCPClientSsl>();
	GoodClient->Construct(Trusted);
	GoodClient->OnConnected.AddDynamic(Good, &UIPTestListener::OnConnected);
	GoodClient->OnMessage.AddDynamic(Good, &UIPTestListener::OnBytes);
	GoodClient->OnError.AddDynamic(Good, &UIPTestListener::OnError);
	GoodClient->Connect(ClientTo(Port, TEXT("localhost")));

	// 2) Trusted CA but wrong host name: must fail.
	FSecurityContextOpts WrongHost = Trusted;
	WrongHost.Host_Name_Verification = TEXT("example.com");
	UIPTestListener* BadHost = Make<UIPTestListener>();
	UTCPClientSsl* BadHostClient = Make<UTCPClientSsl>();
	BadHostClient->Construct(WrongHost);
	BadHostClient->OnConnected.AddDynamic(BadHost, &UIPTestListener::OnConnected);
	BadHostClient->OnError.AddDynamic(BadHost, &UIPTestListener::OnError);
	BadHostClient->OnClose.AddDynamic(BadHost, &UIPTestListener::OnClose);
	BadHostClient->Connect(ClientTo(Port, TEXT("localhost")));

	// 3) Untrusted certificate (default trust store only): must fail.
	UIPTestListener* Untrusted = Make<UIPTestListener>();
	UTCPClientSsl* UntrustedClient = Make<UTCPClientSsl>();
	UntrustedClient->Construct(FSecurityContextOpts());
	UntrustedClient->OnConnected.AddDynamic(Untrusted, &UIPTestListener::OnConnected);
	UntrustedClient->OnError.AddDynamic(Untrusted, &UIPTestListener::OnError);
	UntrustedClient->OnClose.AddDynamic(Untrusted, &UIPTestListener::OnClose);
	UntrustedClient->Connect(ClientTo(Port, TEXT("localhost")));

	WaitUntil(this, TEXT("trusted TLS connection"), [Good]() { return Good->Connected == 1; });
	Then([GoodClient]() { GoodClient->Write(TEXT("secret ç"), FDelegateTcpClientMessageSent()); });
	WaitUntil(this, TEXT("TLS echo"), [Good]() { return Good->Received.Num() >= 9; });
	WaitUntil(this, TEXT("wrong host rejected"), [BadHost]() { return BadHost->Closed == 1; });
	WaitUntil(this, TEXT("untrusted rejected"), [Untrusted]() { return Untrusted->Closed == 1; });
	Then([this, Good, BadHost, Untrusted, GoodClient, BadHostClient, UntrustedClient, Server, ServerL]() {
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Good->Received.GetData()), Good->Received.Num());
		TestEqual(TEXT("TLS echo payload"), FString(Text.Length(), Text.Get()), FString(TEXT("secret ç")));
		TestEqual(TEXT("Wrong host never connects"), BadHost->Connected, 0);
		TestTrue(TEXT("Wrong host reports an error"), BadHost->Errors > 0);
		TestEqual(TEXT("Untrusted cert never connects"), Untrusted->Connected, 0);
		TestTrue(TEXT("Untrusted cert reports an error"), Untrusted->Errors > 0);
		GoodClient->Close();
		Server->Close();
		for (UObject* Object : TArray<UObject*>{Good, BadHost, Untrusted, GoodClient, BadHostClient, UntrustedClient, Server, ServerL}) {
			Object->RemoveFromRoot();
		}
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPWssTest, "InternetProtocol.TLS.SecureWebSocket", IPTests::Flags)
bool FIPWssTest::RunTest(const FString& Parameters) {
	FString CertPem, KeyPem;
	if (!TestTrue(TEXT("Self-signed certificate generated"), MakeSelfSignedCertificate(CertPem, KeyPem))) {
		return false;
	}
	FSecurityContextOpts ServerOpts;
	ServerOpts.Cert = CertPem;
	ServerOpts.Private_Key = KeyPem;
	ServerOpts.Verify_Mode = EVerifyMode::None;
	FSecurityContextOpts ClientOpts;
	ClientOpts.CA_Cert = CertPem;
	ClientOpts.bUseSystemRootCertificates = false;

	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UWSServerSsl* Server = Make<UWSServerSsl>();
	Server->Construct(ServerOpts);
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnWssAccepted);
	UWSClientSsl* Client = Make<UWSClientSsl>();
	Client->Construct(ClientOpts);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnWsConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnWsMessage);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnWsClose);
	if (!TestTrue(TEXT("WSS server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Client->Connect(ClientTo(Server->LocalEndpoint().Port, TEXT("localhost")));
	WaitUntil(this, TEXT("wss handshake"), [L]() { return L->Connected == 1; });
	const TArray<uint8> Data = Pattern(300 * 1024);
	Then([Client, Data]() { Client->WriteBuffer(Data, FDataframe(), FDelegateWsClientMessageSent()); });
	WaitUntil(this, TEXT("wss echo"), [L]() { return L->Messages == 1; });
	Then([this, L, Client, Data]() {
		TestTrue(TEXT("WSS payload intact"), L->Received == Data);
		Client->End(1000, TEXT("done"));
	});
	WaitUntil(this, TEXT("wss closed"), [L]() { return L->Closed == 1; });
	Then([this, L, ServerL, Client, Server]() {
		TestEqual(TEXT("WSS close code"), L->LastCloseCode, 1000);
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// Manual test (not matched by the "InternetProtocol" filter): leaves live connections and requests a graceful
// engine exit, so FInternetProtocolModule::ShutdownModule runs with sessions in flight. Run it alone:
//   -ExecCmds="Automation RunTests IPManual"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPManualExitTest, "IPManual.GracefulExitWithLiveConnections", IPTests::Flags)
bool FIPManualExitTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	UWSServer* WsServer = Make<UWSServer>();
	Server->OnClientAccepted.AddDynamic(L, &UIPTestListener::OnTcpAccepted);
	WsServer->OnClientAccepted.AddDynamic(L, &UIPTestListener::OnWsAccepted);
	Server->Open(Loopback());
	WsServer->Open(Loopback());
	for (int32 Index = 0; Index < 32; ++Index) {
		UTCPClient* Client = Make<UTCPClient>();
		Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
		Client->Connect(ClientTo(Server->LocalEndpoint().Port));
		UWSClient* Ws = Make<UWSClient>();
		Ws->Connect(ClientTo(WsServer->LocalEndpoint().Port));
		UHttpClient* Http = Make<UHttpClient>();
		Http->SetHost(ClientTo(Server->LocalEndpoint().Port));
		Http->Request(FHttpRequest(), FDelegateHttpClientResponse());
	}
	WaitUntil(this, TEXT("connections up"), [L]() { return L->Connected >= 32; });
	Then([]() {
		UE_LOG(LogTemp, Display, TEXT("IPManual: requesting graceful exit with live connections"));
		RequestEngineExit(TEXT("IPManual graceful exit test"));
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// Phase 0 regressions

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPWsCloseReconnectTest, "InternetProtocol.WebSocket.CloseThenConnect", IPTests::Flags)
bool FIPWsCloseReconnectTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UWSServer* Server = Make<UWSServer>();
	UWSClient* Client = Make<UWSClient>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnWsAccepted);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnWsConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnWsMessage);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnWsClose);
	if (!TestTrue(TEXT("WS server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;
	Client->Connect(ClientTo(Port));
	WaitUntil(this, TEXT("first connection"), [L]() { return L->Connected == 1; });
	Then([this, Client, Port]() {
		// The old connection's close arrives after the new connection started: it must not wipe it.
		Client->Close(1000, TEXT("again"));
		TestTrue(TEXT("Reconnect accepted right after Close"), Client->Connect(ClientTo(Port)));
	});
	WaitUntil(this, TEXT("second connection"), [L]() { return L->Connected == 2; });
	WaitUntil(this, TEXT("first OnClose"), [L]() { return L->Closed == 1; });
	Then([Client]() { Client->Write(TEXT("still alive"), FDataframe(), FDelegateWsClientMessageSent()); });
	WaitUntil(this, TEXT("echo on the new connection"), [L]() { return L->Messages == 1; });
	Then([this, L, ServerL, Client, Server]() {
		TestTrue(TEXT("New connection still open"), Client->IsOpen());
		TestEqual(TEXT("Exactly one OnClose (the old connection)"), L->Closed, 1);
		Client->Close(1000, TEXT("done"));
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPTcpCloseReconnectTest, "InternetProtocol.TCP.CloseTwiceKeepsEveryOnClose", IPTests::Flags)
bool FIPTcpCloseReconnectTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	UTCPClient* Client = Make<UTCPClient>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnTcpAccepted);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnClose);
	if (!TestTrue(TEXT("TCP server opens"), Server->Open(Loopback()))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;
	Then([Client, Port]() {
		// Close -> Connect -> Close quickly: both sessions must still report their OnClose.
		Client->Connect(ClientTo(Port));
		Client->Close();
		Client->Connect(ClientTo(Port));
		Client->Close();
	});
	WaitUntil(this, TEXT("two OnClose"), [L]() { return L->Closed == 2; });
	Then([this, L, ServerL, Client, Server]() {
		TestEqual(TEXT("Two OnClose"), L->Closed, 2);
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPServerCloseRemotesTest, "InternetProtocol.TCP.ServerCloseNotifiesRemotes", IPTests::Flags)
bool FIPServerCloseRemotesTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	UTCPClient* Client = Make<UTCPClient>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnClose);
	if (!TestTrue(TEXT("TCP server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Client->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("accepted"), [ServerL]() { return ServerL->Accepted == 1; });
	Then([this, Server]() {
		Server->Close();
		TestEqual(TEXT("Remote still listed until its OnClose"), Server->Clients().Num(), 1);
	});
	WaitUntil(this, TEXT("remote OnClose"), [ServerL]() { return ServerL->RemoteClosed == 1; });
	WaitUntil(this, TEXT("client OnClose"), [L]() { return L->Closed == 1; });
	Then([this, L, ServerL, Client, Server]() {
		TestEqual(TEXT("Remote removed after its OnClose"), Server->Clients().Num(), 0);
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpInterimTest, "InternetProtocol.HTTP.InterimResponses", IPTests::Flags)
bool FIPHttpInterimTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Raw = Make<UTCPServer>();
	UHttpClient* Client = Make<UHttpClient>();
	ServerL->RawReply = TEXT("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n")
	                    TEXT("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal");
	Raw->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted);
	if (!TestTrue(TEXT("Raw server opens"), Raw->Open(Loopback()))) {
		return false;
	}
	Client->SetHost(ClientTo(Raw->LocalEndpoint().Port));
	FDelegateHttpClientResponse OnResponse;
	OnResponse.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpResponse));
	Client->Request(FHttpRequest(), OnResponse);
	WaitUntil(this, TEXT("final response"), [L]() { return L->Responses.Num() >= 1; });
	Then([this, L, Raw, Client]() {
		TestEqual(TEXT("Exactly one response delivered"), L->Responses.Num(), 1);
		TestEqual(TEXT("1xx skipped: final status"), L->Responses[0].Status_Code, 200);
		TestEqual(TEXT("Final body"), L->Responses[0].Body, FString(TEXT("final")));
		Client->Close();
		Raw->Close();
		Release(Client);
		Release(Raw);
		Release(L);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpSetHostInFlightTest, "InternetProtocol.HTTP.SetHostCancelsInFlight", IPTests::Flags)
bool FIPHttpSetHostInFlightTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Silent = Make<UTCPServer>();
	UHttpServer* Real = Make<UHttpServer>();
	UHttpClient* Client = Make<UHttpClient>();
	Silent->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted); // never answers
	FDelegateHttpServerRequest Hello;
	Hello.BindUFunction(ServerL, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpHello));
	Real->Get(TEXT("/hello"), Hello);
	if (!TestTrue(TEXT("Servers open"), Silent->Open(Loopback()) && Real->Open(Loopback()))) {
		return false;
	}
	Client->SetHost(ClientTo(Silent->LocalEndpoint().Port));
	FDelegateHttpClientResponse OnResponse;
	OnResponse.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpResponse));
	Client->Request(FHttpRequest(), OnResponse);
	Client->Request(FHttpRequest(), OnResponse);
	WaitUntil(this, TEXT("request reached the silent server"), [ServerL]() { return ServerL->Messages >= 1; });
	Then([Client, Real, OnResponse]() {
		Client->SetHost(ClientTo(Real->LocalEndpoint().Port));
		FHttpRequest Get;
		Get.Path = TEXT("/hello");
		Client->Request(Get, OnResponse);
	});
	WaitUntil(this, TEXT("cancelled + new response"), [L]() { return L->Responses.Num() == 3; });
	Then([this, L, Silent, Real, Client]() {
		TestTrue(TEXT("In-flight request cancelled"), L->ResponseErrors[0].Value != 0);
		TestTrue(TEXT("Queued request cancelled"), L->ResponseErrors[1].Value != 0);
		TestEqual(TEXT("Request to the new host works"), L->Responses[2].Status_Code, 200);
		Client->Close();
		Silent->Close();
		Real->Close();
		Release(Client);
		Release(Silent);
		Release(Real);
		Release(L);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpMuteHandlerTest, "InternetProtocol.HTTP.MuteHandlerGets503", IPTests::Flags)
bool FIPHttpMuteHandlerTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UHttpServer* Server = Make<UHttpServer>();
	UHttpClient* Client = Make<UHttpClient>();
	Server->ResponseTimeoutSeconds = 0.5f;
	FDelegateHttpServerRequest Mute;
	Mute.BindUFunction(ServerL, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpMute));
	Server->Get(TEXT("/mute"), Mute);
	if (!TestTrue(TEXT("HTTP server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Client->SetHost(ClientTo(Server->LocalEndpoint().Port));
	FDelegateHttpClientResponse OnResponse;
	OnResponse.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpResponse));
	FHttpRequest Get;
	Get.Path = TEXT("/mute");
	Client->Request(Get, OnResponse);
	Client->Request(Get, OnResponse);
	WaitUntil(this, TEXT("two 503"), [L]() { return L->Responses.Num() == 2; }, 10.0);
	Then([this, L, Server, Client]() {
		TestEqual(TEXT("First 503"), L->Responses[0].Status_Code, 503);
		TestEqual(TEXT("Second request answered too (connection not stuck)"), L->Responses[1].Status_Code, 503);
		Client->Close();
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHttpPipelineFloodTest, "InternetProtocol.HTTP.PipelineFloodIsBounded", IPTests::Flags)
bool FIPHttpPipelineFloodTest::RunTest(const FString& Parameters) {
	UIPTestListener* L = Make<UIPTestListener>();
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UHttpServer* Server = Make<UHttpServer>();
	UTCPClient* Raw = Make<UTCPClient>();
	Server->ResponseTimeoutSeconds = 0.0f; // the first request stays unanswered while the flood arrives
	FDelegateHttpServerRequest Mute;
	Mute.BindUFunction(ServerL, GET_FUNCTION_NAME_CHECKED(UIPTestListener, OnHttpMute));
	Server->Get(TEXT("/mute"), Mute);
	Raw->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Raw->OnClose.AddDynamic(L, &UIPTestListener::OnClose);
	if (!TestTrue(TEXT("HTTP server opens"), Server->Open(Loopback()))) {
		return false;
	}
	Raw->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("raw connected"), [L]() { return L->Connected == 1; });
	Then([Raw]() {
		const FString One = TEXT("GET /mute HTTP/1.1\r\nHost: x\r\n\r\n");
		FString Flood;
		while (Flood.Len() < 2 * 1024 * 1024) {
			Flood += One;
		}
		Raw->Write(Flood, FDelegateTcpClientMessageSent());
	});
	WaitUntil(this, TEXT("server drops the flooding connection"), [L]() { return L->Closed == 1; }, 15.0);
	Then([this, ServerL, Server, Raw]() {
		TestEqual(TEXT("Only the first request reached the handler"), ServerL->Messages, 1);
		Server->Close();
		Release(Raw);
		Release(Server);
		Release(ServerL);
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// Phase 1: transport hardening

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPConnectTimeoutTest, "InternetProtocol.Transport.ConnectTimeout", IPTests::Flags)
bool FIPConnectTimeoutTest::RunTest(const FString& Parameters) {
	// A plain TCP server that never answers: the TLS handshake of the client can never complete.
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Silent = Make<UTCPServer>();
	Silent->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted);
	if (!TestTrue(TEXT("Silent server opens"), Silent->Open(Loopback()))) {
		return false;
	}
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPClientSsl* Client = Make<UTCPClientSsl>();
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnError.AddDynamic(L, &UIPTestListener::OnError);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnClose);
	FClientBindOptions Options = ClientTo(Silent->LocalEndpoint().Port);
	Options.ConnectTimeoutSeconds = 1.0f;
	const double Start = FPlatformTime::Seconds();
	Client->Connect(Options);
	WaitUntil(this, TEXT("connect timeout"), [L]() { return L->Closed == 1; }, 10.0);
	Then([this, L, ServerL, Silent, Client, Start]() {
		TestEqual(TEXT("Never connected"), L->Connected, 0);
		TestEqual(TEXT("TimedOut reported"), L->LastError.Message.Contains(TEXT("timed out")), true);
		TestEqual(TEXT("Plugin error category"), L->LastError.Category, EErrorCategory::Plugin);
		TestTrue(TEXT("Within a few seconds"), FPlatformTime::Seconds() - Start < 5.0);
		Silent->Close();
		Release(Client);
		Release(Silent);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPHandshakeTimeoutTest, "InternetProtocol.Transport.HandshakeTimeout", IPTests::Flags)
bool FIPHandshakeTimeoutTest::RunTest(const FString& Parameters) {
	FString CertPem, KeyPem;
	if (!TestTrue(TEXT("Self-signed certificate generated"), MakeSelfSignedCertificate(CertPem, KeyPem))) {
		return false;
	}
	FSecurityContextOpts ServerOpts;
	ServerOpts.Cert = CertPem;
	ServerOpts.Private_Key = KeyPem;
	ServerOpts.Verify_Mode = EVerifyMode::None;
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServerSsl* Server = Make<UTCPServerSsl>();
	Server->Construct(ServerOpts);
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnTcpSslAccepted);
	FServerBindOptions Bind = Loopback();
	Bind.HandshakeTimeoutSeconds = 1.0f;
	if (!TestTrue(TEXT("TLS server opens"), Server->Open(Bind))) {
		return false;
	}
	// A plain TCP client never sends a ClientHello: the server must drop it.
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPClient* Client = Make<UTCPClient>();
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnClose.AddDynamic(L, &UIPTestListener::OnClose);
	Client->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("server drops the silent client"), [L]() { return L->Closed == 1; }, 10.0);
	Then([this, L, ServerL, Server, Client]() {
		TestEqual(TEXT("TCP connected first"), L->Connected, 1);
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPMaxConnectionsTest, "InternetProtocol.Transport.MaxConnections", IPTests::Flags)
bool FIPMaxConnectionsTest::RunTest(const FString& Parameters) {
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted);
	FServerBindOptions Bind = Loopback();
	Bind.MaxConnections = 1;
	if (!TestTrue(TEXT("TCP server opens"), Server->Open(Bind))) {
		return false;
	}
	UIPTestListener* First = Make<UIPTestListener>();
	UIPTestListener* Second = Make<UIPTestListener>();
	UTCPClient* A = Make<UTCPClient>();
	UTCPClient* B = Make<UTCPClient>();
	A->OnClose.AddDynamic(First, &UIPTestListener::OnClose);
	B->OnClose.AddDynamic(Second, &UIPTestListener::OnClose);
	A->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("first accepted"), [ServerL]() { return ServerL->Accepted == 1; });
	Then([B, Server]() { B->Connect(ClientTo(Server->LocalEndpoint().Port)); });
	WaitUntil(this, TEXT("second refused"), [Second]() { return Second->Closed == 1; }, 10.0);
	Then([this, First, Second, ServerL, Server, A, B]() {
		TestEqual(TEXT("Only one connection accepted"), ServerL->Accepted, 1);
		TestEqual(TEXT("First connection still open"), First->Closed, 0);
		TestTrue(TEXT("First client open"), A->IsOpen());
		Server->Close();
		Release(A);
		Release(B);
		Release(Server);
		Release(First);
		Release(Second);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPBackpressureTest, "InternetProtocol.Transport.BackpressureKeepsData", IPTests::Flags)
bool FIPBackpressureTest::RunTest(const FString& Parameters) {
	// 24 MiB through a 1 MiB receive limit: reading pauses and resumes many times, nothing is lost or reordered.
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnRawAccepted);
	FServerBindOptions Bind = Loopback();
	Bind.MaxPendingReceiveMB = 1;
	if (!TestTrue(TEXT("TCP server opens"), Server->Open(Bind))) {
		return false;
	}
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPClient* Client = Make<UTCPClient>();
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnBytes);
	FClientBindOptions Options = ClientTo(Server->LocalEndpoint().Port);
	Options.MaxPendingReceiveMB = 1;
	Client->Connect(Options);
	// The remote is accepted before its own OnConnected: wait until it can send.
	WaitUntil(this, TEXT("connected"), [L, ServerL]() {
		return L->Connected == 1 && ServerL->RawRemotes.Num() == 1 && ServerL->RawRemotes[0]->IsOpen();
	});
	const TArray<uint8> Data = Pattern(24 * 1024 * 1024);
	Then([this, ServerL, Data]() {
		TestTrue(TEXT("24 MiB queued"), ServerL->RawRemotes[0]->WriteBuffer(Data, FDelegateTcpRemoteMessageSent()));
	});
	WaitUntil(this, TEXT("24 MiB received"), [L, Data]() { return L->Received.Num() >= Data.Num(); }, 60.0);
	Then([this, L, ServerL, Server, Client, Data]() {
		AddInfo(FString::Printf(TEXT("Received %d of %d bytes in %d messages"), L->Received.Num(), Data.Num(), L->Messages));
		TestTrue(TEXT("Received intact and in order"), L->Received == Data);
		Client->Close();
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPDualStackTest, "InternetProtocol.Transport.DualStack", IPTests::Flags)
bool FIPDualStackTest::RunTest(const FString& Parameters) {
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UTCPServer* Server = Make<UTCPServer>();
	Server->OnClientAccepted.AddDynamic(ServerL, &UIPTestListener::OnTcpAccepted);
	FServerBindOptions Bind;
	Bind.Address = TEXT("");          // any address
	Bind.Port = 0;
	Bind.Protocol = EProtocolType::Any; // "::" with IPv4 mapped
	if (!TestTrue(TEXT("Dual stack server opens"), Server->Open(Bind))) {
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;
	UIPTestListener* V4 = Make<UIPTestListener>();
	UIPTestListener* V6 = Make<UIPTestListener>();
	UTCPClient* ClientV4 = Make<UTCPClient>();
	UTCPClient* ClientV6 = Make<UTCPClient>();
	ClientV4->OnConnected.AddDynamic(V4, &UIPTestListener::OnConnected);
	ClientV6->OnConnected.AddDynamic(V6, &UIPTestListener::OnConnected);
	ClientV4->Connect(ClientTo(Port, TEXT("127.0.0.1")));
	ClientV6->Connect(ClientTo(Port, TEXT("[::1]")));
	WaitUntil(this, TEXT("IPv4 and IPv6 clients connected"), [V4, V6]() { return V4->Connected == 1 && V6->Connected == 1; });
	Then([this, V4, V6, ServerL, Server, ClientV4, ClientV6]() {
		TestEqual(TEXT("Both accepted"), ServerL->Accepted, 2);
		ClientV4->Close();
		ClientV6->Close();
		Server->Close();
		Release(ClientV4);
		Release(ClientV6);
		Release(Server);
		Release(V4);
		Release(V6);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPUdpLargeDatagramTest, "InternetProtocol.UDP.LargeDatagram", IPTests::Flags)
bool FIPUdpLargeDatagramTest::RunTest(const FString& Parameters) {
	UIPTestListener* ServerL = Make<UIPTestListener>();
	UIPTestListener* L = Make<UIPTestListener>();
	UUDPServer* Server = Make<UUDPServer>();
	UUDPClient* Client = Make<UUDPClient>();
	ServerL->UdpServer = Server;
	Server->OnMessage.AddDynamic(ServerL, &UIPTestListener::OnUdpServerMessage);
	Client->OnConnected.AddDynamic(L, &UIPTestListener::OnConnected);
	Client->OnMessage.AddDynamic(L, &UIPTestListener::OnBytes);
	Client->OnError.AddDynamic(L, &UIPTestListener::OnError);
	if (!TestTrue(TEXT("UDP server binds"), Server->Bind(Loopback()))) {
		return false;
	}
	Client->Connect(ClientTo(Server->LocalEndpoint().Port));
	WaitUntil(this, TEXT("udp connected"), [L]() { return L->Connected == 1; });
	const TArray<uint8> Datagram = Pattern(60 * 1024);
	Then([Client, Datagram]() { Client->SendBuffer(Datagram, FDelegateUdpClientMessageSent()); });
	WaitUntil(this, TEXT("60 KB echo"), [L]() { return L->Messages == 1; });
	Then([this, L, ServerL, Server, Client, Datagram]() {
		TestTrue(TEXT("60 KB datagram not truncated"), L->Received == Datagram);
		TestEqual(TEXT("No error"), L->Errors, 0);
		Client->Close();
		Server->Close();
		Release(Client);
		Release(Server);
		Release(L);
		Release(ServerL);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPErrorCategoryTest, "InternetProtocol.Transport.ErrorCategory", IPTests::Flags)
bool FIPErrorCategoryTest::RunTest(const FString& Parameters) {
	UTCPServer* Probe = Make<UTCPServer>();
	Probe->Open(Loopback());
	const int32 Port = Probe->LocalEndpoint().Port;
	Probe->Close();
	UIPTestListener* L = Make<UIPTestListener>();
	UTCPClient* Client = Make<UTCPClient>();
	Client->OnError.AddDynamic(L, &UIPTestListener::OnError);
	Client->Connect(ClientTo(Port));
	WaitUntil(this, TEXT("connection refused"), [L]() { return L->Errors >= 1; });
	Then([this, L, Client, Probe]() {
		TestEqual(TEXT("Refused is a system error"), L->LastError.Category, EErrorCategory::System);
		Release(Client);
		Release(Probe);
		Release(L);
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
