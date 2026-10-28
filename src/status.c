/* Status code names from §4.6.1.2 ("Status Code") tables. */
#include "nvme_spec.h"
#include "nvmetal/nvmetal.h"

#include <stdio.h>

static const char *generic_name(unsigned sc)
{
    switch (sc) {
    case NVME_SC_SUCCESS:
        return "Successful Completion";
    case NVME_SC_INVALID_OPCODE:
        return "Invalid Command Opcode";
    case NVME_SC_INVALID_FIELD:
        return "Invalid Field in Command";
    case NVME_SC_CID_CONFLICT:
        return "Command ID Conflict";
    case NVME_SC_DATA_XFER_ERROR:
        return "Data Transfer Error";
    case NVME_SC_POWER_LOSS:
        return "Commands Aborted due to Power Loss Notification";
    case NVME_SC_INTERNAL:
        return "Internal Error";
    case NVME_SC_ABORT_REQ:
        return "Command Abort Requested";
    case NVME_SC_ABORT_SQ_DELETED:
        return "Command Aborted due to SQ Deletion";
    case NVME_SC_INVALID_NS:
        return "Invalid Namespace or Format";
    case NVME_SC_PRP_OFFSET_INVALID:
        return "PRP Offset Invalid";
    case NVME_SC_LBA_RANGE:
        return "LBA Out of Range";
    case NVME_SC_CAP_EXCEEDED:
        return "Capacity Exceeded";
    case NVME_SC_NS_NOT_READY:
        return "Namespace Not Ready";
    default:
        return NULL;
    }
}

static const char *cmd_specific_name(unsigned sc)
{
    switch (sc) {
    case NVME_SC_CQ_INVALID:
        return "Completion Queue Invalid";
    case NVME_SC_QID_INVALID:
        return "Invalid Queue Identifier";
    case NVME_SC_QUEUE_SIZE:
        return "Invalid Queue Size";
    case NVME_SC_INVALID_LOG_PAGE:
        return "Invalid Log Page";
    case NVME_SC_INVALID_FORMAT:
        return "Invalid Format";
    case NVME_SC_INVALID_QDELETE:
        return "Invalid Queue Deletion";
    default:
        return NULL;
    }
}

static const char *media_name(unsigned sc)
{
    switch (sc) {
    case NVME_SC_WRITE_FAULT:
        return "Write Fault";
    case NVME_SC_READ_ERROR:
        return "Unrecovered Read Error";
    case NVME_SC_GUARD_CHECK:
        return "End-to-end Guard Check Error";
    case NVME_SC_APPTAG_CHECK:
        return "End-to-end Application Tag Check Error";
    case NVME_SC_REFTAG_CHECK:
        return "End-to-end Reference Tag Check Error";
    case NVME_SC_ACCESS_DENIED:
        return "Access Denied";
    default:
        return NULL;
    }
}

const char *nvm_status_str(uint32_t status, char *buf, size_t len)
{
    if (status == NVM_STATUS_TIMEOUT) {
        snprintf(buf, len, "Command Timeout (driver)");
        return buf;
    }
    if (status == NVM_STATUS_ABORTED) {
        snprintf(buf, len, "Aborted by Queue Teardown (driver)");
        return buf;
    }
    unsigned sct = NVM_STATUS_SCT(status), sc = NVM_STATUS_SC(status);
    const char *name = NULL;
    switch (sct) {
    case NVME_SCT_GENERIC:
        name = generic_name(sc);
        break;
    case NVME_SCT_CMD_SPEC:
        name = cmd_specific_name(sc);
        break;
    case NVME_SCT_MEDIA:
        name = media_name(sc);
        break;
    case NVME_SCT_PATH:
        name = "Path Related Status";
        break;
    case NVME_SCT_VENDOR:
        name = "Vendor Specific";
        break;
    default:
        break;
    }
    snprintf(buf, len, "%s (sct %u sc 0x%02x%s)", name ? name : "Unknown Status", sct, sc,
             NVM_STATUS_DNR(status) ? ", dnr" : "");
    return buf;
}
