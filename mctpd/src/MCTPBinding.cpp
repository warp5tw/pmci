#include "MCTPBinding.hpp"

#include <systemd/sd-id128.h>

#include <phosphor-logging/log.hpp>

#include "libmctp-cmds.h"
#include "libmctp-msgtypes.h"
#include "libmctp.h"

constexpr sd_id128_t mctpdAppId = SD_ID128_MAKE(c4, e4, d9, 4a, 88, 43, 4d, f0,
                                                94, 9d, bb, 0a, af, 53, 4e, 6d);
constexpr unsigned int ctrlTxPollInterval = 5;
constexpr size_t minCmdRespSize = 4;
constexpr int completionCodeIndex = 3;

// map<EID, assigned>
static std::unordered_map<mctp_eid_t, bool> eidPoolMap;
bool ctrlTxTimerExpired = true;
// <state, retryCount, maxRespDelay, destEid, BindingPrivate, ReqPacket,
//  Callback>
static std::vector<
    std::tuple<PacketState, uint8_t, unsigned int, mctp_eid_t,
               std::vector<uint8_t>, std::vector<uint8_t>,
               std::function<void(PacketState, std::vector<uint8_t>&)>>>
    ctrlTxQueue;

static uint8_t getInstanceId(const uint8_t msg)
{
    return msg & MCTP_CTRL_HDR_INSTANCE_ID_MASK;
}

static void handleCtrlResp(void* msg, const size_t len)
{
    mctp_ctrl_msg_hdr* respHeader = reinterpret_cast<mctp_ctrl_msg_hdr*>(msg);

    auto reqItr =
        std::find_if(ctrlTxQueue.begin(), ctrlTxQueue.end(), [&](auto& ctrlTx) {
            auto& [state, retryCount, maxRespDelay, destEid, bindingPrivate,
                   req, callback] = ctrlTx;

            mctp_ctrl_msg_hdr* reqHeader =
                reinterpret_cast<mctp_ctrl_msg_hdr*>(req.data());

            // TODO: Check Message terminus with Instance ID
            // (EID, TO, Msg Tag) + Instance ID
            if (getInstanceId(reqHeader->rq_dgram_inst) ==
                getInstanceId(respHeader->rq_dgram_inst))
            {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "Matching Control command request found");

                uint8_t* tmp = reinterpret_cast<uint8_t*>(msg);
                std::vector<uint8_t> resp =
                    std::vector<uint8_t>(tmp, tmp + len);
                state = PacketState::receivedResponse;

                // Call Callback function
                callback(state, resp);
                return true;
            }
            return false;
        });

    if (reqItr != ctrlTxQueue.end())
    {
        // Delete the entry from queue after receiving response
        ctrlTxQueue.erase(reqItr);
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "No matching Control command request found");
    }
}

void rxMessage(uint8_t srcEid, void* /*data*/, void* msg, size_t len,
               void* /*binding_private*/)
{
    uint8_t* payload = reinterpret_cast<uint8_t*>(msg);
    uint8_t msgType = payload[0]; // Always the first byte
    uint8_t msgTag = 0;           // Currently libmctp doesn't expose msgTag
    bool tagOwner = false;
    std::vector<uint8_t> response;

    response.assign(payload, payload + len);

    if (msgType != MCTP_MESSAGE_TYPE_MCTP_CTRL)
    {
        auto msgSignal =
            conn->new_signal("/xyz/openbmc_project/mctp",
                             mctp_server::interface, "MessageReceivedSignal");
        msgSignal.append(msgType, srcEid, msgTag, tagOwner, response);
        msgSignal.signal_send();
    }

    if (mctp_is_mctp_ctrl_msg(msg, len) && !mctp_ctrl_msg_is_req(msg, len))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "MCTP Control packet response received!!");
        handleCtrlResp(msg, len);
    }
}

bool MctpBinding::getBindingPrivateData(uint8_t /*dstEid*/,
                                        std::vector<uint8_t>& pvtData)
{
    // No Binding data by default
    pvtData.clear();
    return true;
}

MctpBinding::MctpBinding(std::shared_ptr<object_server>& objServer,
                         std::string& objPath, ConfigurationVariant& conf,
                         boost::asio::io_context& ioc) :
    io(ioc),
    objectServer(objServer), ctrlTxTimer(io)
{
    mctpInterface = objServer->add_interface(objPath, mctp_server::interface);

    try
    {
        if (SMBusConfiguration* smbusConf =
                std::get_if<SMBusConfiguration>(&conf))
        {
            ownEid = smbusConf->defaultEid;
            bindingID = smbusConf->bindingType;
            bindingMediumID = smbusConf->mediumId;
            bindingModeType = smbusConf->mode;
            ctrlTxRetryDelay = smbusConf->reqToRespTime;
            ctrlTxRetryCount = smbusConf->reqRetryCount;

            // TODO: Add bus owner interface.
            // TODO: If we are not top most busowner, wait for top mostbus owner
            // to issue EID Pool
            if (smbusConf->mode == mctp_server::BindingModeTypes::BusOwner)
            {
                initializeEidPool(smbusConf->eidPool);
            }
        }
        else if (PcieConfiguration* pcieConf =
                     std::get_if<PcieConfiguration>(&conf))
        {
            ownEid = pcieConf->defaultEid;
            bindingID = pcieConf->bindingType;
            bindingMediumID = pcieConf->mediumId;
            bindingModeType = pcieConf->mode;
            ctrlTxRetryDelay = pcieConf->reqToRespTime;
            ctrlTxRetryCount = pcieConf->reqRetryCount;
        }
        else
        {
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument));
        }

        createUuid();
        registerProperty(mctpInterface, "Eid", ownEid);

        registerProperty(mctpInterface, "StaticEid", staticEid);

        registerProperty(mctpInterface, "Uuid", uuid);

        registerProperty(mctpInterface, "BindingID",
                         mctp_server::convertBindingTypesToString(bindingID));

        registerProperty(
            mctpInterface, "BindingMediumID",
            mctp_server::convertMctpPhysicalMediumIdentifiersToString(
                bindingMediumID));

        registerProperty(
            mctpInterface, "BindingMode",
            mctp_server::convertBindingModeTypesToString(bindingModeType));

        /*
         * msgTag and tagOwner are not currently used, but can't be removed
         * since they are defined for SendMctpMessagePayload() in the current
         * version of MCTP D-Bus interface.
         */
        mctpInterface->register_method(
            "SendMctpMessagePayload",
            [this](uint8_t dstEid, [[maybe_unused]] uint8_t msgTag,
                   [[maybe_unused]] bool tagOwner,
                   std::vector<uint8_t> payload) {
                std::vector<uint8_t> pvtData;

                getBindingPrivateData(dstEid, pvtData);

                return mctp_message_tx(mctp, dstEid, payload.data(),
                                       payload.size(), pvtData.data());
            });

        mctpInterface->register_signal<uint8_t, uint8_t, uint8_t, bool,
                                       std::vector<uint8_t>>(
            "MessageReceivedSignal");

        if (mctpInterface->initialize() == false)
        {
            throw std::system_error(
                std::make_error_code(std::errc::function_not_supported));
        }
    }
    catch (std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MCTP Interface initialization failed.",
            phosphor::logging::entry("Exception:", e.what()));
        throw;
    }
}

MctpBinding::~MctpBinding()
{
    objectServer->remove_interface(mctpInterface);
    if (mctp)
    {
        mctp_destroy(mctp);
    }
}

void MctpBinding::createUuid(void)
{
    sd_id128_t id;

    if (sd_id128_get_machine_app_specific(mctpdAppId, &id))
    {
        throw std::system_error(
            std::make_error_code(std::errc::address_not_available));
    }

    uuid.insert(uuid.begin(), std::begin(id.bytes), std::end(id.bytes));
    if (uuid.size() != 16)
    {
        throw std::system_error(std::make_error_code(std::errc::bad_address));
    }
}

void MctpBinding::initializeMctp(void)
{
    mctp_set_log_stdio(MCTP_LOG_INFO);
    mctp = mctp_init();
    if (!mctp)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to init mctp");
        throw std::system_error(
            std::make_error_code(std::errc::not_enough_memory));
    }
    mctp_set_rx_all(mctp, rxMessage, nullptr);
}

void MctpBinding::initializeEidPool(const std::vector<mctp_eid_t>& pool)
{
    for (auto const& epId : pool)
    {
        eidPoolMap.emplace(epId, false);
    }
}

void MctpBinding::updateEidStatus(const mctp_eid_t endpointId,
                                  const bool assigned)
{
    auto eidItr = eidPoolMap.find(endpointId);
    if (eidItr != eidPoolMap.end())
    {
        eidItr->second = assigned;
        if (assigned)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("EID " + std::to_string(endpointId) + " is assigned").c_str());
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("EID " + std::to_string(endpointId) + " added to pool")
                    .c_str());
        }
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Unable to find EID " + std::to_string(endpointId) +
             " in the pool")
                .c_str());
    }
}

mctp_eid_t MctpBinding::getAvailableEidFromPool(void)
{
    // Note:- No need to check for busowner role explicitly when accessing EID
    // pool since getAvailableEidFromPool will be called only in busowner mode.

    for (auto& eidPair : eidPoolMap)
    {
        if (!eidPair.second)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("Allocated EID: " + std::to_string(eidPair.first)).c_str());
            eidPair.second = true;
            return eidPair.first;
        }
    }
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "No free EID in the pool");
    throw std::system_error(
        std::make_error_code(std::errc::address_not_available));
}

bool MctpBinding::sendMctpMessage(mctp_eid_t destEid, std::vector<uint8_t> req,
                                  std::vector<uint8_t> bindingPrivate)
{
    if (mctp_message_tx(mctp, destEid, req.data(), req.size(),
                        bindingPrivate.data()) < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Error in mctp_message_tx");
        return false;
    }
    return true;
}

void MctpBinding::processCtrlTxQueue(void)
{
    ctrlTxTimerExpired = false;
    ctrlTxTimer.expires_after(std::chrono::milliseconds(ctrlTxPollInterval));
    ctrlTxTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted do nothing
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "ctrlTxTimer operation_aborted");
            return;
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "ctrlTxTimer failed");
            return;
        }

        // Discard the packet if retry count exceeded

        ctrlTxQueue.erase(
            std::remove_if(
                ctrlTxQueue.begin(), ctrlTxQueue.end(),
                [this](auto& ctrlTx) {
                    auto& [state, retryCount, maxRespDelay, destEid,
                           bindingPrivate, req, callback] = ctrlTx;

                    maxRespDelay -= ctrlTxPollInterval;

                    // If no reponse:
                    // Retry the packet on every ctrlTxRetryDelay
                    // Total no of tries = 1 + ctrlTxRetryCount
                    if (maxRespDelay > 0 &&
                        state != PacketState::receivedResponse)
                    {
                        if (retryCount > 0 &&
                            maxRespDelay <= retryCount * ctrlTxRetryDelay)
                        {
                            if (sendMctpMessage(destEid, req, bindingPrivate))
                            {
                                phosphor::logging::log<
                                    phosphor::logging::level::INFO>(
                                    "Packet transmited");
                                state = PacketState::transmitted;
                            }

                            // Decrement retry count
                            retryCount--;
                        }

                        return false;
                    }

                    state = PacketState::noResponse;
                    std::vector<uint8_t> resp1 = {};
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "Retry timed out, No response");

                    // Call Callback function
                    callback(state, resp1);
                    return true;
                }),
            ctrlTxQueue.end());

        if (ctrlTxQueue.empty())
        {
            ctrlTxTimer.cancel();
            ctrlTxTimerExpired = true;
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "ctrlTxQueue empty, canceling timer");
        }
        else
        {
            processCtrlTxQueue();
        }
    });
}

void MctpBinding::pushToCtrlTxQueue(
    PacketState state, const mctp_eid_t destEid,
    const std::vector<uint8_t>& bindingPrivate, const std::vector<uint8_t>& req,
    std::function<void(PacketState, std::vector<uint8_t>&)>& callback)
{
    ctrlTxQueue.push_back(std::make_tuple(
        state, ctrlTxRetryCount, ((ctrlTxRetryCount + 1) * ctrlTxRetryDelay),
        destEid, bindingPrivate, req, callback));

    if (sendMctpMessage(destEid, req, bindingPrivate))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Packet transmited");
        state = PacketState::transmitted;
    }

    if (ctrlTxTimerExpired)
    {
        processCtrlTxQueue();
    }
}

PacketState MctpBinding::sendAndRcvMctpCtrl(
    boost::asio::yield_context& yield, const std::vector<uint8_t>& req,
    const mctp_eid_t destEid, const std::vector<uint8_t>& bindingPrivate,
    std::vector<uint8_t>& resp)
{
    if (req.empty())
    {
        return PacketState::invalidPacket;
    }

    PacketState pktState = PacketState::pushedForTransmission;
    boost::system::error_code ec;
    boost::asio::steady_timer timer(io);

    std::function<void(PacketState, std::vector<uint8_t>&)> callback =
        [&resp, &pktState, &timer](PacketState state,
                                   std::vector<uint8_t>& response) {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Callback triggered");

            resp = response;
            pktState = state;
            timer.cancel();

            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("Packet state: " + std::to_string(static_cast<int>(pktState)))
                    .c_str());
        };

    pushToCtrlTxQueue(pktState, destEid, bindingPrivate, req, callback);

    do
    {
        timer.expires_after(std::chrono::milliseconds(ctrlTxRetryDelay));
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "sendAndRcvMctpCtrl: Timer created, ctrl cmd waiting");
        timer.async_wait(yield[ec]);
        if (ec && ec != boost::asio::error::operation_aborted)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "sendAndRcvMctpCtrl: async_wait error");
        }
    } while (pktState == PacketState::pushedForTransmission);
    // Wait for the state to change

    return pktState;
}

static uint8_t createInstanceId(void)
{
    static uint8_t instanceId = 0x00;

    instanceId = (instanceId + 1) & MCTP_CTRL_HDR_INSTANCE_ID_MASK;
    return instanceId;
}

static uint8_t getRqDgramInst(void)
{
    uint8_t instanceID = createInstanceId();
    uint8_t rqDgramInst = instanceID | MCTP_CTRL_HDR_FLAG_REQUEST;
    return rqDgramInst;
}

template <int cmd, typename... Args>
bool MctpBinding::getFormattedReq(std::vector<uint8_t>& req, Args&&... reqParam)
{
    if constexpr (cmd == MCTP_CTRL_CMD_GET_ENDPOINT_ID)
    {
        req.resize(sizeof(mctp_ctrl_cmd_get_eid));
        mctp_ctrl_cmd_get_eid* getEidCmd =
            reinterpret_cast<mctp_ctrl_cmd_get_eid*>(req.data());

        mctp_encode_ctrl_cmd_get_eid(getEidCmd, getRqDgramInst());
        return true;
    }
    else if constexpr (cmd == MCTP_CTRL_CMD_SET_ENDPOINT_ID)
    {
        req.resize(sizeof(mctp_ctrl_cmd_set_eid));
        mctp_ctrl_cmd_set_eid* setEidCmd =
            reinterpret_cast<mctp_ctrl_cmd_set_eid*>(req.data());

        mctp_encode_ctrl_cmd_set_eid(setEidCmd, getRqDgramInst(),
                                     std::forward<Args>(reqParam)...);
        return true;
    }
    else if constexpr (cmd == MCTP_CTRL_CMD_GET_ENDPOINT_UUID)
    {
        req.resize(sizeof(mctp_ctrl_cmd_get_uuid));
        mctp_ctrl_cmd_get_uuid* getUuid =
            reinterpret_cast<mctp_ctrl_cmd_get_uuid*>(req.data());

        mctp_encode_ctrl_cmd_get_uuid(getUuid, getRqDgramInst());
        return true;
    }
    else if constexpr (cmd == MCTP_CTRL_CMD_GET_VERSION_SUPPORT)
    {
        req.resize(sizeof(mctp_ctrl_cmd_get_mctp_ver_support));
        mctp_ctrl_cmd_get_mctp_ver_support* getVerSupport =
            reinterpret_cast<mctp_ctrl_cmd_get_mctp_ver_support*>(req.data());

        mctp_encode_ctrl_cmd_get_ver_support(getVerSupport, getRqDgramInst(),
                                             std::forward<Args>(reqParam)...);
        return true;
    }

    else if constexpr (cmd == MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT)
    {
        req.resize(sizeof(mctp_ctrl_cmd_get_msg_type_support));
        mctp_ctrl_cmd_get_msg_type_support* getMsgType =
            reinterpret_cast<mctp_ctrl_cmd_get_msg_type_support*>(req.data());

        mctp_encode_ctrl_cmd_get_msg_type_support(getMsgType, getRqDgramInst());
        return true;
    }
    else if constexpr (cmd == MCTP_CTRL_CMD_GET_VENDOR_MESSAGE_SUPPORT)
    {
        req.resize(sizeof(mctp_ctrl_cmd_get_vdm_support));
        mctp_ctrl_cmd_get_vdm_support* getVdmSupport =
            reinterpret_cast<mctp_ctrl_cmd_get_vdm_support*>(req.data());

        mctp_encode_ctrl_cmd_get_vdm_support(getVdmSupport, getRqDgramInst(),
                                             std::forward<Args>(reqParam)...);
        return true;
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Control command not defined");
        return false;
    }
}

static bool checkMinRespSize(const std::vector<uint8_t>& resp)
{
    return (resp.size() >= minCmdRespSize);
}

template <typename structure>
static bool checkRespSizeAndCompletionCode(std::vector<uint8_t>& resp)
{
    if (!checkMinRespSize(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid response length");
        return false;
    }

    structure* respPtr = reinterpret_cast<structure*>(resp.data());

    if (respPtr->completion_code != MCTP_CTRL_CC_SUCCESS ||
        resp.size() != sizeof(structure))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid response", phosphor::logging::entry("LEN=%d", resp.size()),
            phosphor::logging::entry("CC=0x%02X", respPtr->completion_code));
        return false;
    }
    return true;
}

bool MctpBinding::getEidCtrlCmd(boost::asio::yield_context& yield,
                                const std::vector<uint8_t>& bindingPrivate,
                                const mctp_eid_t destEid,
                                std::vector<uint8_t>& resp)
{
    std::vector<uint8_t> req = {};

    if (!getFormattedReq<MCTP_CTRL_CMD_GET_ENDPOINT_ID>(req))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get EID: Request formatting failed");
        return false;
    }

    if (PacketState::receivedResponse !=
        sendAndRcvMctpCtrl(yield, req, destEid, bindingPrivate, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get EID: Unable to get response");
        return false;
    }

    if (!checkRespSizeAndCompletionCode<mctp_ctrl_resp_get_eid>(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>("Get EID failed");
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>("Get EID success");
    return true;
}

bool MctpBinding::setEidCtrlCmd(boost::asio::yield_context& yield,
                                const std::vector<uint8_t>& bindingPrivate,
                                const mctp_eid_t destEid,
                                const mctp_ctrl_cmd_set_eid_op operation,
                                mctp_eid_t eid, std::vector<uint8_t>& resp)
{
    std::vector<uint8_t> req = {};

    if (!getFormattedReq<MCTP_CTRL_CMD_SET_ENDPOINT_ID>(req, operation, eid))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Set EID: Request formatting failed");
        return false;
    }

    if (PacketState::receivedResponse !=
        sendAndRcvMctpCtrl(yield, req, destEid, bindingPrivate, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Set EID: Unable to get response");
        return false;
    }

    if (!checkRespSizeAndCompletionCode<mctp_ctrl_resp_set_eid>(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>("Set EID failed");
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>("Set EID success");
    return true;
}

bool MctpBinding::getUuidCtrlCmd(boost::asio::yield_context& yield,
                                 const std::vector<uint8_t>& bindingPrivate,
                                 const mctp_eid_t destEid,
                                 std::vector<uint8_t>& resp)
{
    std::vector<uint8_t> req = {};

    if (!getFormattedReq<MCTP_CTRL_CMD_GET_ENDPOINT_UUID>(req))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get UUID: Request formatting failed");
        return false;
    }

    if (PacketState::receivedResponse !=
        sendAndRcvMctpCtrl(yield, req, destEid, bindingPrivate, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get UUID: Unable to get response");
        return false;
    }

    if (!checkRespSizeAndCompletionCode<mctp_ctrl_resp_get_uuid>(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get UUID failed");
        return false;
    }

    phosphor::logging::log<phosphor::logging::level::INFO>("Get UUID success");
    return true;
}

bool MctpBinding::getMsgTypeSupportCtrlCmd(
    boost::asio::yield_context& yield,
    const std::vector<uint8_t>& bindingPrivate, const mctp_eid_t destEid,
    MsgTypeSupportCtrlResp* msgTypeSupportResp)
{
    std::vector<uint8_t> req = {};
    std::vector<uint8_t> resp = {};

    if (!getFormattedReq<MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT>(req))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get Message Type Support: Request formatting failed");
        return false;
    }

    if (PacketState::receivedResponse !=
        sendAndRcvMctpCtrl(yield, req, destEid, bindingPrivate, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get Message Type Support: Unable to get response");
        return false;
    }

    if (!checkMinRespSize(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get Message Type Support: Invalid response");
        return false;
    }

    const size_t minMsgTypeRespLen = 5;
    uint8_t completionCode = resp[completionCodeIndex];
    if (completionCode != MCTP_CTRL_CC_SUCCESS ||
        resp.size() <= minMsgTypeRespLen)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get Message Type Support: Invalid response",
            phosphor::logging::entry("CC=0x%02X", completionCode),
            phosphor::logging::entry("LEN=0x%02X", resp.size()));

        std::vector<uint8_t> respHeader =
            std::vector<uint8_t>(resp.begin(), resp.begin() + minCmdRespSize);
        std::copy(
            respHeader.begin(), respHeader.end(),
            reinterpret_cast<uint8_t*>(&msgTypeSupportResp->ctrlMsgHeader));
        msgTypeSupportResp->completionCode = completionCode;
        return false;
    }

    std::copy_n(resp.begin(), minMsgTypeRespLen,
                reinterpret_cast<uint8_t*>(msgTypeSupportResp));
    if ((resp.size() - minMsgTypeRespLen) != msgTypeSupportResp->msgTypeCount)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get Message Type Support: Invalid response length");
        return false;
    }

    msgTypeSupportResp->msgType.assign(resp.begin() + minMsgTypeRespLen,
                                       resp.end());

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Get Message Type Support success");
    return true;
}

bool MctpBinding::getMctpVersionSupportCtrlCmd(
    boost::asio::yield_context& yield,
    const std::vector<uint8_t>& bindingPrivate, const mctp_eid_t destEid,
    const uint8_t msgTypeNo,
    MctpVersionSupportCtrlResp* mctpVersionSupportCtrlResp)
{
    std::vector<uint8_t> req = {};
    std::vector<uint8_t> resp = {};

    if (!getFormattedReq<MCTP_CTRL_CMD_GET_VERSION_SUPPORT>(req, msgTypeNo))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get MCTP Version Support: Request formatting failed");
        return false;
    }

    if (PacketState::receivedResponse !=
        sendAndRcvMctpCtrl(yield, req, destEid, bindingPrivate, resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get MCTP Version Support: Unable to get response");
        return false;
    }

    if (!checkMinRespSize(resp))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get MCTP Version Support: Invalid response");
        return false;
    }

    const ssize_t minMsgTypeRespLen = 5;
    const ssize_t mctpVersionLen = 4;
    uint8_t completionCode = resp[completionCodeIndex];
    if (completionCode != MCTP_CTRL_CC_SUCCESS ||
        resp.size() <= minMsgTypeRespLen)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get MCTP Version Support: Invalid response",
            phosphor::logging::entry("CC=0x%02X", completionCode),
            phosphor::logging::entry("LEN=0x%02X", resp.size()));

        std::vector<uint8_t> respHeader =
            std::vector<uint8_t>(resp.begin(), resp.begin() + minCmdRespSize);
        std::copy(respHeader.begin(), respHeader.end(),
                  reinterpret_cast<uint8_t*>(
                      &mctpVersionSupportCtrlResp->ctrlMsgHeader));
        mctpVersionSupportCtrlResp->completionCode = completionCode;
        return false;
    }

    std::copy_n(resp.begin(), minMsgTypeRespLen,
                reinterpret_cast<uint8_t*>(mctpVersionSupportCtrlResp));
    if ((resp.size() - minMsgTypeRespLen) !=
        mctpVersionSupportCtrlResp->verNoEntryCount * mctpVersionLen)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Get MCTP Version Support: Invalid response length");
        return false;
    }

    for (int iter = 1; iter <= mctpVersionSupportCtrlResp->verNoEntryCount;
         iter++)
    {
        ssize_t verNoEntryStartOffset =
            minMsgTypeRespLen + (mctpVersionLen * (iter - 1));
        ssize_t verNoEntryEndOffset =
            minMsgTypeRespLen + (mctpVersionLen * iter);
        std::vector<uint8_t> version(resp.begin() + verNoEntryStartOffset,
                                     resp.begin() + verNoEntryEndOffset);

        mctpVersionSupportCtrlResp->verNoEntry.push_back(version);
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Get MCTP Version Support success");
    return true;
}

void MctpBinding::registerMsgTypes(
    std::shared_ptr<sdbusplus::asio::dbus_interface>& msgTypeIntf,
    const MsgTypes& messageType)
{
    msgTypeIntf->register_property("MctpControl", messageType.mctpControl);
    msgTypeIntf->register_property("PLDM", messageType.pldm);
    msgTypeIntf->register_property("NCSI", messageType.ncsi);
    msgTypeIntf->register_property("Ethernet", messageType.ethernet);
    msgTypeIntf->register_property("NVMeMgmtMsg", messageType.nvmeMgmtMsg);
    msgTypeIntf->register_property("SPDM", messageType.spdm);
    msgTypeIntf->register_property("VDPCI", messageType.vdpci);
    msgTypeIntf->register_property("VDIANA", messageType.vdiana);
    msgTypeIntf->initialize();
}

void MctpBinding::populateEndpointProperties(
    const EndpointProperties& epProperties)
{

    std::string mctpDevObj = "/xyz/openbmc_project/mctp/device/";
    std::shared_ptr<sdbusplus::asio::dbus_interface> endpointIntf;
    std::string mctpEpObj =
        mctpDevObj + std::to_string(epProperties.endpointEid);

    // Endpoint interface
    endpointIntf =
        objectServer->add_interface(mctpEpObj, mctp_endpoint::interface);
    endpointIntf->register_property(
        "Mode",
        mctp_server::convertBindingModeTypesToString(epProperties.mode));
    endpointIntf->register_property("NetworkId", epProperties.networkId);
    endpointIntf->initialize();
    endpointInterface.push_back(endpointIntf);

    // Message type interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> msgTypeIntf;
    msgTypeIntf =
        objectServer->add_interface(mctpEpObj, mctp_msg_types::interface);
    registerMsgTypes(msgTypeIntf, epProperties.endpointMsgTypes);
    msgTypeInterface.push_back(msgTypeIntf);

    // UUID interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> uuidIntf;
    uuidIntf = objectServer->add_interface(mctpEpObj,
                                           "xyz.openbmc_project.Common.UUID");
    uuidIntf->register_property("UUID", epProperties.uuid);
    uuidIntf->initialize();
    uuidInterface.push_back(uuidIntf);
}

mctp_server::BindingModeTypes MctpBinding::getEndpointType(const uint8_t types)
{
    constexpr uint8_t endpointTypeMask = 0x30;
    constexpr int endpointTypeShift = 0x04;
    constexpr uint8_t simpleEndpoint = 0x00;
    constexpr uint8_t busOwnerBridge = 0x01;

    uint8_t endpointType = (types & endpointTypeMask) >> endpointTypeShift;

    if (endpointType == simpleEndpoint)
    {
        return mctp_server::BindingModeTypes::Endpoint;
    }
    else if (endpointType == busOwnerBridge)
    {
        // TODO: need to differentiate between BusOwner and Bridge
        return mctp_server::BindingModeTypes::Bridge;
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Invalid endpoint type value");
        throw;
    }
}

MsgTypes MctpBinding::getMsgTypes(const std::vector<uint8_t>& msgType)
{
    MsgTypes messageTypes;

    for (auto type : msgType)
    {
        switch (type)
        {
            case MCTP_MESSAGE_TYPE_MCTP_CTRL: {
                messageTypes.mctpControl = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_PLDM: {
                messageTypes.pldm = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_NCSI: {
                messageTypes.ncsi = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_ETHERNET: {
                messageTypes.ethernet = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_NVME: {
                messageTypes.nvmeMgmtMsg = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_SPDM: {
                messageTypes.spdm = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_VDPCI: {
                messageTypes.vdpci = true;
                break;
            }
            case MCTP_MESSAGE_TYPE_VDIANA: {
                messageTypes.vdiana = true;
                break;
            }
            default: {
                // TODO: Add OEM Message Type support
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Invalid message type");
                break;
            }
        }
    }
    return messageTypes;
}

static std::string formatUUID(guid_t& uuid)
{
    const size_t safeBufferLength = 50;
    char buf[safeBufferLength] = {0};
    auto ptr = reinterpret_cast<uint8_t*>(&uuid);

    snprintf(
        buf, safeBufferLength,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7], ptr[8],
        ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15]);
    // UUID is in RFC4122 format. Ex: 61a39523-78f2-11e5-9862-e6402cfc3223
    return std::string(buf);
}

void MctpBinding::busOwnerRegisterEndpoint(
    const std::vector<uint8_t>& bindingPrivate)
{
    boost::asio::spawn(io, [bindingPrivate,
                            this](boost::asio::yield_context yield) {
        // Get EID
        std::vector<uint8_t> getEidResp = {};
        if (!(getEidCtrlCmd(yield, bindingPrivate, 0x00, getEidResp)))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Get EID failed");
            return;
        }

        mctp_ctrl_resp_get_eid* getEidRespPtr =
            reinterpret_cast<mctp_ctrl_resp_get_eid*>(getEidResp.data());
        mctp_eid_t destEid = getEidRespPtr->eid;

        if (getEidRespPtr->eid != 0x00)
        {
            updateEidStatus(destEid, true);
        }

        // Get UUID (Not mandatory to support)
        std::vector<uint8_t> getUuidResp = {};
        if (!(getUuidCtrlCmd(yield, bindingPrivate, destEid, getUuidResp)))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Get UUID failed");
        }

        // TODO: Check the obtained UUID from the route table and verify whether
        // it had an entry in the route table
        // TODO: Routing table construction
        // TODO: Assigne pool of EID if the endpoint is a bridge
        // TODO: Wait for T-reclame to free an EID
        // TODO: Take care of EIDs(Static EID) which are not owned by us

        // Set EID
        if (getEidRespPtr->eid == 0x00)
        {
            mctp_eid_t eid;
            try
            {
                eid = getAvailableEidFromPool();
            }
            catch (const std::exception&)
            {
                return;
            }
            std::vector<uint8_t> setEidResp = {};
            if (!(setEidCtrlCmd(yield, bindingPrivate, 0x00, set_eid, eid,
                                setEidResp)))
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Set EID failed");
                updateEidStatus(eid, false);
                return;
            }

            mctp_ctrl_resp_set_eid* setEidRespPtr =
                reinterpret_cast<mctp_ctrl_resp_set_eid*>(setEidResp.data());
            destEid = setEidRespPtr->eid_set;

            // If EID in the resp is different from the one sent in request,
            // we need to check if that EID exists in the pool and update its
            // status as assigned.
            updateEidStatus(destEid, true);
        }

        // Get Message Type Support
        MsgTypeSupportCtrlResp msgTypeSupportResp;
        if (!(getMsgTypeSupportCtrlCmd(yield, bindingPrivate, destEid,
                                       &msgTypeSupportResp)))
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Get Message Type Support failed");
            return;
        }

        // TODO: Get Vendor ID command

        // Expose interface as per the result
        EndpointProperties epProperties;
        epProperties.endpointEid = destEid;
        mctp_ctrl_resp_get_uuid* getUuidRespPtr =
            reinterpret_cast<mctp_ctrl_resp_get_uuid*>(getUuidResp.data());
        epProperties.uuid = formatUUID(getUuidRespPtr->uuid);
        try
        {
            epProperties.mode = getEndpointType(getEidRespPtr->eid_type);
        }
        catch (const std::exception&)
        {
            return;
        }
        // Network ID need to be assigned only if EP is requesting for the same.
        // Keep Network ID as zero and update it later if a change happend.
        epProperties.networkId = 0x00;
        epProperties.endpointMsgTypes = getMsgTypes(msgTypeSupportResp.msgType);
        populateEndpointProperties(epProperties);
    });
}

void MctpBinding::registerEndpoint(const std::vector<uint8_t>& bindingPrivate,
                                   bool isBusOwner)
{
    if (isBusOwner)
    {
        busOwnerRegisterEndpoint(bindingPrivate);
    }
    // TODO: Control command flow if we are not busowner
}
