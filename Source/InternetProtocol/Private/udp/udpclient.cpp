/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "udp/udpclient.h"
#include "net/facade.h"

using IPNet::FFacadeAccess;

void UUDPClient::BeginDestroy() {
	FFacadeAccess::ReleaseUdp(Net);
	Super::BeginDestroy();
}

void UUDPClient::AddToRoot() {
	Super::AddToRoot();
}

void UUDPClient::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UUDPClient::IsRooted() {
	return Super::IsRooted();
}

bool UUDPClient::IsOpen() {
	return Net.bOpen;
}

FUdpEndpoint UUDPClient::LocalEndpoint() {
	return Net.Local;
}

FUdpEndpoint UUDPClient::RemoteEndpoint() {
	return Net.Remote;
}

void UUDPClient::SetRecvBufferSize(int Val) {
	// A datagram can never exceed 65507 bytes of payload.
	recv_buffer_size = FMath::Clamp(Val, 1, 65536);
	if (Net.Socket) {
		Net.Socket->SetReceiveBufferSize(static_cast<size_t>(recv_buffer_size));
	}
}

int UUDPClient::GetRecvBufferSize() {
	return recv_buffer_size;
}

FErrorCode UUDPClient::GetErrorCode() {
	return Net.LastError;
}

bool UUDPClient::Send(const FString& Message, const FDelegateUdpClientMessageSent& Callback) {
	if (!Net.Socket || !Net.bOpen || Message.IsEmpty()) {
		return false;
	}
	return Net.Socket->Send(IPNet::ToUtf8(Message), IPNet::MakeSendCallback(this, Callback));
}

bool UUDPClient::SendBuffer(const TArray<uint8>& Buffer, const FDelegateUdpClientMessageSent& Callback) {
	if (!Net.Socket || !Net.bOpen || Buffer.Num() == 0) {
		return false;
	}
	return Net.Socket->Send(IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), IPNet::MakeSendCallback(this, Callback));
}

bool UUDPClient::Connect(const FClientBindOptions& BindOpts) {
	if (Net.Socket) {
		return false;
	}
	const uint64 Id = IPNet::NextSessionId();
	IPNet::FUdpEvents Events = FFacadeAccess::MakeUdpEvents(this, Id,
		[](UUDPClient* Self, const TArray<uint8>& Buffer, const asio::ip::udp::endpoint&) {
			Self->OnMessage.Broadcast(Buffer, Buffer.Num());
		});
	std::shared_ptr<IPNet::FUdpSocket> Socket = IPNet::FUdpSocket::Create(MoveTemp(Events), static_cast<size_t>(recv_buffer_size));
	if (!Socket) {
		FFacadeAccess::FailImmediately(this, IPNet::EError::NotRunning);
		return false;
	}
	Net.Socket = Socket;
	Net.SessionId = Id;
	Net.bOpen = false;
	Net.Local = FUdpEndpoint();
	Net.Remote = FUdpEndpoint();
	Net.LastError = FErrorCode();
	Socket->Connect(IPNet::ToStd(BindOpts.Address), IPNet::ToStd(BindOpts.Port), BindOpts.Protocol);
	return true;
}

void UUDPClient::Close() {
	FFacadeAccess::CloseUdp(Net);
}
