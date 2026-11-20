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
	std::shared_ptr<IPNet::FFlowControl> Flow = IPNet::MakeFlow(BindOpts.MaxPendingReceiveMB);
	IPNet::FUdpEvents Events = FFacadeAccess::MakeUdpEvents(this, Id,
		[](UUDPClient* Self, const TArray<uint8>& Buffer, const asio::ip::udp::endpoint&) {
			Self->OnMessage.Broadcast(Buffer, Buffer.Num());
		}, Flow);
	std::shared_ptr<IPNet::FUdpSocket> Socket = IPNet::FUdpSocket::Create(MoveTemp(Events), static_cast<size_t>(recv_buffer_size));
	if (!Socket) {
		FFacadeAccess::FailImmediately(this, IPNet::EError::NotRunning);
		return false;
	}
	Socket->SetFlowControl(Flow);
	Socket->SetSocketBuffers(BindOpts.SocketReceiveBufferBytes, BindOpts.SocketSendBufferBytes);
	if (!BindOpts.LocalAddress.IsEmpty() || BindOpts.LocalPort != 0) {
		asio::ip::address Local;
		if (!IPNet::MakeListenAddress(BindOpts.LocalAddress, BindOpts.Protocol == EProtocolType::V6 ? EProtocolType::V6 : EProtocolType::V4, Local) ||
		    BindOpts.LocalPort < 0 || BindOpts.LocalPort > 65535) {
			Socket->Close();
			FFacadeAccess::FailImmediately(this, IPNet::EError::InvalidArgument);
			return false;
		}
		Socket->SetLocalEndpoint(asio::ip::udp::endpoint(Local, static_cast<uint16>(BindOpts.LocalPort)));
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

namespace {
	bool ParseGroup(const FString& Text, asio::ip::address& Out) {
		asio::error_code Ec;
		Out = asio::ip::make_address(TCHAR_TO_UTF8(*Text), Ec);
		return !Ec && Out.is_multicast();
	}
}

void UUDPClient::SetBroadcast(bool bEnable) {
	if (Net.Socket) {
		Net.Socket->SetBroadcast(bEnable);
	}
}

bool UUDPClient::JoinMulticastGroup(const FString& GroupAddress) {
	asio::ip::address Group;
	if (!Net.Socket || !ParseGroup(GroupAddress, Group)) {
		return false;
	}
	Net.Socket->JoinGroup(Group);
	return true;
}

bool UUDPClient::LeaveMulticastGroup(const FString& GroupAddress) {
	asio::ip::address Group;
	if (!Net.Socket || !ParseGroup(GroupAddress, Group)) {
		return false;
	}
	Net.Socket->LeaveGroup(Group);
	return true;
}

void UUDPClient::SetMulticastTTL(int32 Hops) {
	if (Net.Socket) {
		Net.Socket->SetMulticastTtl(Hops);
	}
}

void UUDPClient::SetMulticastLoopback(bool bEnable) {
	if (Net.Socket) {
		Net.Socket->SetMulticastLoopback(bEnable);
	}
}
