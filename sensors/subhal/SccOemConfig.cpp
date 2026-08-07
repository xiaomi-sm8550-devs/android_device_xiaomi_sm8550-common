/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "SccOemConfig"

#include "SccOemConfig.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <vector>

// ABI declarations checked against the supplied ARM64 libssc.so.
struct sensor_uid {
    uint64_t low;
    uint64_t high;
};

enum ssc_connection_type {
    SSC_CONNECTION_QMI = 0,
};

enum ssc_interface_error {
    SSC_INTERFACE_ERROR_NONE = 0,
};

struct ssc_conn_config {
    uint64_t opaque;
};

class ssc_interface {
  public:
    static ssc_interface* create(ssc_connection_type type, ssc_conn_config config);
};

class ssc_qmi {
  public:
    ~ssc_qmi();
    bool send_request(sensor_uid suid, bool sync, std::string payload);
    void register_cb(
            sensor_uid suid, std::function<void(unsigned int, unsigned long)> onResp,
            std::function<void(ssc_interface_error)> onError,
            std::function<void(const unsigned char*, unsigned long, unsigned long)> onEvent);
};

class suid_lookup {
  public:
    suid_lookup(std::function<void(const std::string&, const std::vector<sensor_uid>&)> cb);
    ~suid_lookup();
    void request_suid(std::string datatype, bool register_updates);

  private:
    // ARM64 libssc uses libc++ function storage requiring 16-byte alignment.
    alignas(16) char opaque_[256];
};

namespace {
constexpr uint32_t kMsgIdOemConfig = 0x800;

enum OemConfigField : uint32_t {
    FIELD_TYPE = 1,
    FIELD_BACKLIGHT = 9,
    FIELD_DC_STATE = 10,
    FIELD_LUX = 11,
    FIELD_AUX = 12,
    FIELD_DISPLAY_FREQ = 15,
};

enum ClientRequestField : uint32_t {
    REQ_SUID = 1,
    REQ_MSG_ID = 2,
    REQ_SUSP_CONFIG = 3,
    REQ_REQUEST = 4,
};

enum SuidField : uint32_t {
    SUID_LOW = 1,
    SUID_HIGH = 2,
};

enum SuspendConfigField : uint32_t {
    SUSP_CLIENT_PROC_TYPE = 1,
    SUSP_DELIVERY_TYPE = 2,
};

enum StdRequestField : uint32_t {
    REQUEST_PAYLOAD = 2,
};

constexpr uint32_t kClientProcTypeApss = 1;
constexpr uint32_t kDeliveryTypeNoWakeup = 1;
constexpr char kDataType[] = "ambient_light";
constexpr auto kSuidTimeout = std::chrono::seconds(2);

enum WireType : uint32_t {
    WIRE_VARINT = 0,
    WIRE_FIXED64 = 1,
    WIRE_LENGTH_DELIMITED = 2,
    WIRE_FIXED32 = 5,
};

void putVarint(std::string* out, uint64_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        out->push_back(static_cast<char>(byte));
    } while (value);
}

void putTag(std::string* out, uint32_t field, uint32_t wireType) {
    putVarint(out, (field << 3) | wireType);
}

void putVarintField(std::string* out, uint32_t field, uint64_t value) {
    putTag(out, field, WIRE_VARINT);
    putVarint(out, value);
}

void putFixed64Field(std::string* out, uint32_t field, uint64_t value) {
    putTag(out, field, WIRE_FIXED64);
    for (int i = 0; i < 8; i++) {
        out->push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

void putFixed32Field(std::string* out, uint32_t field, uint32_t value) {
    putTag(out, field, WIRE_FIXED32);
    for (int i = 0; i < 4; i++) {
        out->push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

void putBytesField(std::string* out, uint32_t field, const std::string& value) {
    putTag(out, field, WIRE_LENGTH_DELIMITED);
    putVarint(out, value.size());
    out->append(value);
}

}  // namespace

SccOemConfig::SccOemConfig() = default;

SccOemConfig::~SccOemConfig() {
    delete reinterpret_cast<suid_lookup*>(mLookup);
}

SccOemConfig& SccOemConfig::getInstance() {
    static SccOemConfig instance;
    return instance;
}

bool SccOemConfig::ensureReady() {
    if (mReady) {
        return true;
    }

    if (!lookupSuid()) {
        return false;
    }

    if (mConnection == nullptr) {
        ssc_conn_config config = {};
        mConnection = ssc_interface::create(SSC_CONNECTION_QMI, config);
        if (mConnection == nullptr) {
            LOG(ERROR) << "failed to open SSC connection";
            return false;
        }
        sensor_uid uid = {mSuidLow, mSuidHigh};
        reinterpret_cast<ssc_qmi*>(mConnection)
                ->register_cb(
                        uid,
                        [](unsigned int resp, unsigned long ts) {
                            if (resp != 0) {
                                LOG(ERROR) << "ssc resp=" << resp << " ts=" << ts;
                            } else {
                                LOG(VERBOSE) << "ssc resp=" << resp << " ts=" << ts;
                            }
                        },
                        [](ssc_interface_error error) {
                            LOG(ERROR) << "ssc error=" << static_cast<int>(error);
                        },
                        [](const unsigned char* data, unsigned long len, unsigned long ts) {
                            std::string hex;
                            for (unsigned long i = 0; i < len && i < 64; i++) {
                                android::base::StringAppendF(&hex, "%02x", data[i]);
                            }
                            LOG(INFO) << "ssc event len=" << len << " ts=" << ts << " " << hex;
                        });
    }

    mReady = true;
    return true;
}

bool SccOemConfig::lookupSuid() {
    std::mutex suidMutex;
    std::condition_variable suidCv;
    bool resolved = false;

    auto onSuid = [&](const std::string& datatype, const std::vector<sensor_uid>& suids) {
        if (datatype != kDataType || suids.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(suidMutex);
            mSuidLow = suids[0].low;
            mSuidHigh = suids[0].high;
            resolved = true;
        }
        suidCv.notify_one();
    };

    suid_lookup lookup(onSuid);
    lookup.request_suid(kDataType, false /* register_updates */);

    std::unique_lock<std::mutex> lock(suidMutex);
    if (!suidCv.wait_for(lock, kSuidTimeout, [&] { return resolved; })) {
        LOG(ERROR) << "timed out resolving SUID for " << kDataType;
        return false;
    }

    LOG(INFO) << "resolved " << kDataType << " suid low=" << mSuidLow << " high=" << mSuidHigh;
    return true;
}

bool SccOemConfig::send(int32_t type, const std::string& oemConfig) {
    std::string suid;
    putFixed64Field(&suid, SUID_LOW, mSuidLow);
    putFixed64Field(&suid, SUID_HIGH, mSuidHigh);
    std::string suspendConfig;
    putVarintField(&suspendConfig, SUSP_CLIENT_PROC_TYPE, kClientProcTypeApss);
    putVarintField(&suspendConfig, SUSP_DELIVERY_TYPE, kDeliveryTypeNoWakeup);
    std::string request;
    putBytesField(&request, REQUEST_PAYLOAD, oemConfig);
    std::string encoded;
    putBytesField(&encoded, REQ_SUID, suid);
    putFixed32Field(&encoded, REQ_MSG_ID, kMsgIdOemConfig);
    putBytesField(&encoded, REQ_SUSP_CONFIG, suspendConfig);
    putBytesField(&encoded, REQ_REQUEST, request);
    sensor_uid uid = {mSuidLow, mSuidHigh};
    bool sent = reinterpret_cast<ssc_qmi*>(mConnection)->send_request(uid, true, encoded);
    if (!sent) {
        LOG(ERROR) << "failed to send config type " << type;
        mReady = false;
    }
    return sent;
}

static void putFloatField(std::string* out, uint32_t field, float value) {
    putTag(out, field, WIRE_FIXED32);
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 4; i++) {
        out->push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    }
}

bool SccOemConfig::reportValue(float value, float aux) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!ensureReady()) {
        return false;
    }

    std::string config;
    putVarintField(&config, FIELD_TYPE, BOLED_DATA);
    putFloatField(&config, FIELD_LUX, value);
    putFloatField(&config, FIELD_AUX, aux);

    LOG(VERBOSE) << "report value=" << value << " aux=" << aux;
    return send(BOLED_DATA, config);
}

bool SccOemConfig::notifyBacklight(int32_t brightness) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!ensureReady()) {
        return false;
    }

    std::string config;
    putVarintField(&config, FIELD_TYPE, BACKLIGHT);
    putVarintField(&config, FIELD_BACKLIGHT,
                   static_cast<uint64_t>(static_cast<int64_t>(brightness)));

    LOG(VERBOSE) << "backlight " << brightness;
    return send(BACKLIGHT, config);
}

bool SccOemConfig::notifyDcState(int32_t state) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!ensureReady()) {
        return false;
    }

    std::string config;
    putVarintField(&config, FIELD_TYPE, DC_STATE);
    putVarintField(&config, FIELD_DC_STATE, static_cast<uint64_t>(static_cast<int64_t>(state)));

    LOG(VERBOSE) << "dc state " << state;
    return send(DC_STATE, config);
}

bool SccOemConfig::notifyDisplayFreq(uint32_t freq) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!ensureReady()) {
        return false;
    }

    std::string config;
    putVarintField(&config, FIELD_TYPE, DISPLAY_FREQ);
    putVarintField(&config, FIELD_DISPLAY_FREQ, freq);

    LOG(VERBOSE) << "display freq " << freq;
    return send(DISPLAY_FREQ, config);
}
