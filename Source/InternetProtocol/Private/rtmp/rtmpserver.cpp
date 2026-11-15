/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmpserver.h"
#include "rtmp/rtmpserversession.h"
#include "net/acceptor.h"
#include "net/errors.h"
#include "net/facade.h"
#include "net/gamethread.h"

using namespace IPNet;
using namespace IPNet::Rtmp;

void URTMPServer::BeginDestroy() {
	if (Core) {
		Core->Close();
		Core.reset();
	}
	SessionId = 0;
	PendingCloseId = 0;
	Super::BeginDestroy();
}

void URTMPServer::AddToRoot() {
	Super::AddToRoot();
}

void URTMPServer::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool URTMPServer::IsRooted() {
	return Super::IsRooted();
}

bool URTMPServer::IsOpen() {
	return bOpen;
}

FTcpEndpoint URTMPServer::LocalEndpoint() {
	return Local;
}

FErrorCode URTMPServer::GetErrorCode() {
	return LastError;
}

void URTMPServer::SetAllowedStreamKeys(const TArray<FString>& StreamKeys) {
	AllowedKeys = StreamKeys;
	if (Core) {
		Core->SetAllowedStreamNames(AllowedKeys);
	}
}

bool URTMPServer::Open(const FServerBindOptions& BindOpts) {
	if (Core) {
		return false;
	}
	const uint64 Id = NextSessionId();
	TWeakObjectPtr<URTMPServer> Weak(this);

	FRtmpServerCallbacks Callbacks;
	Callbacks.OnPublishStarted = [Weak, Id](const FRtmpStreamInfo& Info) {
		RunOnGameThread(Weak, [Id, Info](URTMPServer* Self) {
			if (Self->SessionId == Id) {
				Self->OnPublishStarted.Broadcast(Info.App, Info.StreamName, Info.Remote);
			}
		});
	};
	Callbacks.OnPublishEnded = [Weak, Id](const FRtmpStreamInfo& Info) {
		RunOnGameThread(Weak, [Id, Info](URTMPServer* Self) {
			if (Self->SessionId == Id || Self->PendingCloseId == Id) {
				Self->OnPublishEnded.Broadcast(Info.App, Info.StreamName, Info.Remote);
			}
		});
	};
	Callbacks.OnError = [Weak, Id](const asio::error_code& Ec) {
		RunOnGameThread(Weak, [Id, Ec](URTMPServer* Self) {
			if (Self->SessionId == Id) {
				Self->LastError = FErrorCode(Ec);
				Self->OnError.Broadcast(Self->LastError);
			}
		});
	};
	Callbacks.OnClosed = [Weak, Id]() {
		RunOnGameThread(Weak, [Id](URTMPServer* Self) {
			if (Self->SessionId == Id || Self->PendingCloseId == Id) {
				if (Self->SessionId == Id) {
					Self->SessionId = 0;
					Self->bOpen = false;
					Self->Core.reset();
				} else {
					Self->PendingCloseId = 0;
				}
				Self->OnClose.Broadcast();
			}
		});
	};

	asio::ip::address Address;
	asio::error_code Ec;
	if (!MakeListenAddress(BindOpts.Address, BindOpts.Protocol, Address) || BindOpts.Port < 0 || BindOpts.Port > 65535) {
		Ec = MakeError(EError::InvalidArgument);
	}
	std::shared_ptr<FRtmpServerCore> NewCore = FRtmpServerCore::Create(MoveTemp(Callbacks));
	NewCore->SetAllowedStreamNames(AllowedKeys);
	if (!Ec) {
		NewCore->Open(asio::ip::tcp::endpoint(Address, static_cast<uint16>(BindOpts.Port)), BindOpts.bReuse_Address, Backlog, Ec);
	}
	if (Ec) {
		LastError = FErrorCode(Ec);
		OnError.Broadcast(LastError);
		return false;
	}

	Core = NewCore;
	SessionId = Id;
	bOpen = true;
	Local = FTcpEndpoint(Core->GetLocalEndpoint());
	LastError = FErrorCode();
	OnListening.Broadcast();
	return true;
}

void URTMPServer::Close() {
	if (!Core) {
		return;
	}
	PendingCloseId = SessionId;
	SessionId = 0;
	bOpen = false;
	Core->Close();
	Core.reset();
}
