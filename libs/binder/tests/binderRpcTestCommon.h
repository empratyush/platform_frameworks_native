/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <BinderRpcTestClientInfo.h>
#include <BinderRpcTestServerConfig.h>
#include <BinderRpcTestServerInfo.h>
#include <BnBinderRpcCallback.h>
#include <BnBinderRpcSession.h>
#include <BnBinderRpcTest.h>
#include <aidl/IBinderRpcTest.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_auto_utils.h>
#include <android/binder_libbinder.h>
#include <binder/Binder.h>
#include <binder/BpBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/RpcServer.h>
#include <binder/RpcSession.h>
#include <binder/RpcThreads.h>
#include <binder/RpcTlsTestUtils.h>
#include <binder/RpcTlsUtils.h>
#include <binder/RpcTransport.h>
#include <binder/RpcTransportRaw.h>
#include <binder/RpcTransportTls.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <signal.h>

#include "../BuildFlags.h"
#include "../FdTrigger.h"
#include "../OS.h"               // for testing UnixBootstrap clients
#include "../RpcSocketAddress.h" // for testing preconnected clients
#include "../RpcState.h"         // for debugging
#include "../vm_sockets.h"       // for VMADDR_*
#include "utils/Errors.h"

namespace android {

constexpr char kLocalInetAddress[] = "127.0.0.1";

enum class RpcSecurity { RAW, TLS };

static inline std::vector<RpcSecurity> RpcSecurityValues() {
    return {RpcSecurity::RAW, RpcSecurity::TLS};
}

enum class SocketType {
    PRECONNECTED,
    UNIX,
    UNIX_BOOTSTRAP,
    VSOCK,
    INET,
};

static inline std::string PrintToString(SocketType socketType) {
    switch (socketType) {
        case SocketType::PRECONNECTED:
            return "preconnected_uds";
        case SocketType::UNIX:
            return "unix_domain_socket";
        case SocketType::UNIX_BOOTSTRAP:
            return "unix_domain_socket_bootstrap";
        case SocketType::VSOCK:
            return "vm_socket";
        case SocketType::INET:
            return "inet_socket";
        default:
            LOG_ALWAYS_FATAL("Unknown socket type");
            return "";
    }
}

struct BinderRpcOptions {
    size_t numThreads = 1;
    size_t numSessions = 1;
    size_t numIncomingConnections = 0;
    size_t numOutgoingConnections = SIZE_MAX;
    RpcSession::FileDescriptorTransportMode clientFileDescriptorTransportMode =
            RpcSession::FileDescriptorTransportMode::NONE;
    std::vector<RpcSession::FileDescriptorTransportMode>
            serverSupportedFileDescriptorTransportModes = {
                    RpcSession::FileDescriptorTransportMode::NONE};

    // If true, connection failures will result in `ProcessSession::sessions` being empty
    // instead of a fatal error.
    bool allowConnectFailure = false;
};

static inline void writeString(android::base::borrowed_fd fd, std::string_view str) {
    uint64_t length = str.length();
    CHECK(android::base::WriteFully(fd, &length, sizeof(length)));
    CHECK(android::base::WriteFully(fd, str.data(), str.length()));
}

static inline std::string readString(android::base::borrowed_fd fd) {
    uint64_t length;
    CHECK(android::base::ReadFully(fd, &length, sizeof(length)));
    std::string ret(length, '\0');
    CHECK(android::base::ReadFully(fd, ret.data(), length));
    return ret;
}

static inline void writeToFd(android::base::borrowed_fd fd, const Parcelable& parcelable) {
    Parcel parcel;
    CHECK_EQ(OK, parcelable.writeToParcel(&parcel));
    writeString(fd, std::string(reinterpret_cast<const char*>(parcel.data()), parcel.dataSize()));
}

template <typename T>
static inline T readFromFd(android::base::borrowed_fd fd) {
    std::string data = readString(fd);
    Parcel parcel;
    CHECK_EQ(OK, parcel.setData(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    T object;
    CHECK_EQ(OK, object.readFromParcel(&parcel));
    return object;
}

static inline std::unique_ptr<RpcTransportCtxFactory> newFactory(
        RpcSecurity rpcSecurity, std::shared_ptr<RpcCertificateVerifier> verifier = nullptr,
        std::unique_ptr<RpcAuth> auth = nullptr) {
    switch (rpcSecurity) {
        case RpcSecurity::RAW:
            return RpcTransportCtxFactoryRaw::make();
        case RpcSecurity::TLS: {
            if (verifier == nullptr) {
                verifier = std::make_shared<RpcCertificateVerifierSimple>();
            }
            if (auth == nullptr) {
                auth = std::make_unique<RpcAuthSelfSigned>();
            }
            return RpcTransportCtxFactoryTls::make(std::move(verifier), std::move(auth));
        }
        default:
            LOG_ALWAYS_FATAL("Unknown RpcSecurity %d", rpcSecurity);
    }
}

// Create an FD that returns `contents` when read.
static inline base::unique_fd mockFileDescriptor(std::string contents) {
    android::base::unique_fd readFd, writeFd;
    CHECK(android::base::Pipe(&readFd, &writeFd)) << strerror(errno);
    RpcMaybeThread([writeFd = std::move(writeFd), contents = std::move(contents)]() {
        signal(SIGPIPE, SIG_IGN); // ignore possible SIGPIPE from the write
        if (!WriteStringToFd(contents, writeFd)) {
            int savedErrno = errno;
            LOG_ALWAYS_FATAL_IF(EPIPE != savedErrno, "mockFileDescriptor write failed: %s",
                                strerror(savedErrno));
        }
    }).detach();
    return readFd;
}

using android::binder::Status;

class MyBinderRpcSession : public BnBinderRpcSession {
public:
    static std::atomic<int32_t> gNum;

    MyBinderRpcSession(const std::string& name) : mName(name) { gNum++; }
    Status getName(std::string* name) override {
        *name = mName;
        return Status::ok();
    }
    ~MyBinderRpcSession() { gNum--; }

private:
    std::string mName;
};

class MyBinderRpcCallback : public BnBinderRpcCallback {
    Status sendCallback(const std::string& value) {
        RpcMutexUniqueLock _l(mMutex);
        mValues.push_back(value);
        _l.unlock();
        mCv.notify_one();
        return Status::ok();
    }
    Status sendOnewayCallback(const std::string& value) { return sendCallback(value); }

public:
    RpcMutex mMutex;
    RpcConditionVariable mCv;
    std::vector<std::string> mValues;
};

class MyBinderRpcTest : public BnBinderRpcTest {
public:
    wp<RpcServer> server;
    int port = 0;

    Status sendString(const std::string& str) override {
        (void)str;
        return Status::ok();
    }
    Status doubleString(const std::string& str, std::string* strstr) override {
        *strstr = str + str;
        return Status::ok();
    }
    Status getClientPort(int* out) override {
        *out = port;
        return Status::ok();
    }
    Status countBinders(std::vector<int32_t>* out) override {
        sp<RpcServer> spServer = server.promote();
        if (spServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        out->clear();
        for (auto session : spServer->listSessions()) {
            size_t count = session->state()->countBinders();
            out->push_back(count);
        }
        return Status::ok();
    }
    Status getNullBinder(sp<IBinder>* out) override {
        out->clear();
        return Status::ok();
    }
    Status pingMe(const sp<IBinder>& binder, int32_t* out) override {
        if (binder == nullptr) {
            std::cout << "Received null binder!" << std::endl;
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        *out = binder->pingBinder();
        return Status::ok();
    }
    Status repeatBinder(const sp<IBinder>& binder, sp<IBinder>* out) override {
        *out = binder;
        return Status::ok();
    }
    static sp<IBinder> mHeldBinder;
    Status holdBinder(const sp<IBinder>& binder) override {
        mHeldBinder = binder;
        return Status::ok();
    }
    Status getHeldBinder(sp<IBinder>* held) override {
        *held = mHeldBinder;
        return Status::ok();
    }
    Status nestMe(const sp<IBinderRpcTest>& binder, int count) override {
        if (count <= 0) return Status::ok();
        return binder->nestMe(this, count - 1);
    }
    Status alwaysGiveMeTheSameBinder(sp<IBinder>* out) override {
        static sp<IBinder> binder = new BBinder;
        *out = binder;
        return Status::ok();
    }
    Status openSession(const std::string& name, sp<IBinderRpcSession>* out) override {
        *out = new MyBinderRpcSession(name);
        return Status::ok();
    }
    Status getNumOpenSessions(int32_t* out) override {
        *out = MyBinderRpcSession::gNum;
        return Status::ok();
    }

    RpcMutex blockMutex;
    Status lock() override {
        blockMutex.lock();
        return Status::ok();
    }
    Status unlockInMsAsync(int32_t ms) override {
        usleep(ms * 1000);
        blockMutex.unlock();
        return Status::ok();
    }
    Status lockUnlock() override {
        RpcMutexLockGuard _l(blockMutex);
        return Status::ok();
    }

    Status sleepMs(int32_t ms) override {
        usleep(ms * 1000);
        return Status::ok();
    }

    Status sleepMsAsync(int32_t ms) override {
        // In-process binder calls are asynchronous, but the call to this method
        // is synchronous wrt its client. This in/out-process threading model
        // diffentiation is a classic binder leaky abstraction (for better or
        // worse) and is preserved here the way binder sockets plugs itself
        // into BpBinder, as nothing is changed at the higher levels
        // (IInterface) which result in this behavior.
        return sleepMs(ms);
    }

    Status doCallback(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                      const std::string& value) override {
        if (callback == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }

        if (delayed) {
            RpcMaybeThread([=]() {
                ALOGE("Executing delayed callback: '%s'", value.c_str());
                Status status = doCallback(callback, oneway, false, value);
                ALOGE("Delayed callback status: '%s'", status.toString8().c_str());
            }).detach();
            return Status::ok();
        }

        if (oneway) {
            return callback->sendOnewayCallback(value);
        }

        return callback->sendCallback(value);
    }

    Status doCallbackAsync(const sp<IBinderRpcCallback>& callback, bool oneway, bool delayed,
                           const std::string& value) override {
        return doCallback(callback, oneway, delayed, value);
    }

    Status die(bool cleanup) override {
        if (cleanup) {
            exit(1);
        } else {
            _exit(1);
        }
    }

    Status scheduleShutdown() override {
        sp<RpcServer> strongServer = server.promote();
        if (strongServer == nullptr) {
            return Status::fromExceptionCode(Status::EX_NULL_POINTER);
        }
        RpcMaybeThread([=] {
            LOG_ALWAYS_FATAL_IF(!strongServer->shutdown(), "Could not shutdown");
        }).detach();
        return Status::ok();
    }

    Status useKernelBinderCallingId() override {
        // this is WRONG! It does not make sense when using RPC binder, and
        // because it is SO wrong, and so much code calls this, it should abort!

        if constexpr (kEnableKernelIpc) {
            (void)IPCThreadState::self()->getCallingPid();
        }
        return Status::ok();
    }

    Status echoAsFile(const std::string& content, android::os::ParcelFileDescriptor* out) override {
        out->reset(mockFileDescriptor(content));
        return Status::ok();
    }

    Status concatFiles(const std::vector<android::os::ParcelFileDescriptor>& files,
                       android::os::ParcelFileDescriptor* out) override {
        std::string acc;
        for (const auto& file : files) {
            std::string result;
            CHECK(android::base::ReadFdToString(file.get(), &result));
            acc.append(result);
        }
        out->reset(mockFileDescriptor(acc));
        return Status::ok();
    }
};

} // namespace android
