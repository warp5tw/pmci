#ifndef BASE_H
#define BASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <asm/byteorder.h>
#include <stddef.h>
#include <stdint.h>

#include "pldm_types.h"
#include "utils.h"

/** @brief PLDM Commands
 */
enum pldm_supported_commands {
	PLDM_SET_TID = 0x01,
	PLDM_GET_TID = 0x2,
	PLDM_GET_PLDM_VERSION = 0x3,
	PLDM_GET_PLDM_TYPES = 0x4,
	PLDM_GET_PLDM_COMMANDS = 0x5
};

/** @brief PLDM base codes
 */
enum pldm_completion_codes {
	PLDM_SUCCESS = 0x00,
	PLDM_ERROR = 0x01,
	PLDM_ERROR_INVALID_DATA = 0x02,
	PLDM_ERROR_INVALID_LENGTH = 0x03,
	PLDM_ERROR_NOT_READY = 0x04,
	PLDM_ERROR_UNSUPPORTED_PLDM_CMD = 0x05,
	PLDM_ERROR_INVALID_PLDM_TYPE = 0x20,
	PLDM_INVALID_TRANSFER_OPERATION_FLAG = 0x21
};

enum transfer_op_flag {
	PLDM_GET_NEXTPART = 0,
	PLDM_GET_FIRSTPART = 1,
};

enum transfer_resp_flag {
	PLDM_START = 0x01,
	PLDM_MIDDLE = 0x02,
	PLDM_END = 0x04,
	PLDM_START_AND_END = 0x05,
};

/** @enum MessageType
 *
 *  The different message types supported by the PLDM specification.
 */
typedef enum {
	PLDM_RESPONSE,		   //!< PLDM response
	PLDM_REQUEST,		   //!< PLDM request
	PLDM_RESERVED,		   //!< Reserved
	PLDM_ASYNC_REQUEST_NOTIFY, //!< Unacknowledged PLDM request messages
} MessageType;

typedef uint8_t pldm_tid_t;
typedef uint8_t pldm_type_t;

#define PLDM_RQ_D_MASK 0xC0
#define PLDM_RQ_D_SHIFT 0x06
#define PLDM_MSG_TYPE_MASK 0x3F
#define PLDM_TID_MAX 0xFF
#define PLDM_INSTANCE_ID_MASK 0x1F
#define PLDM_INSTANCE_MAX 32
#define PLDM_MAX_TYPES 64
#define PLDM_MAX_CMDS_PER_TYPE 256

/* Message payload lengths */
#define PLDM_GET_COMMANDS_REQ_BYTES 5
#define PLDM_GET_VERSION_REQ_BYTES 6

/* Response lengths are inclusive of completion code */
#define PLDM_GET_TYPES_RESP_BYTES 9
#define PLDM_GET_TID_RESP_BYTES 2
#define PLDM_SET_TID_RESP_BYTES 1
#define PLDM_GET_COMMANDS_RESP_BYTES 33

/* Fixed bytes in GetVersion response not including verison data*/
#define PLDM_GET_VERSION_RESP_FIXED_BYTES 6

#define PLDM_VERSION_0 0
#define PLDM_CURRENT_VERSION PLDM_VERSION_0

/** @struct pldm_msg_hdr
 *
 * Structure representing PLDM message header fields
 */
struct pldm_msg_hdr {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	uint8_t instance_id : 5; //!< Instance ID
	uint8_t reserved : 1;	 //!< Reserved
	uint8_t datagram : 1;	 //!< Datagram bit
	uint8_t request : 1;	 //!< Request bit
#elif defined(__BIG_ENDIAN_BITFIELD)
	uint8_t request : 1;	 //!< Request bit
	uint8_t datagram : 1;	 //!< Datagram bit
	uint8_t reserved : 1;	 //!< Reserved
	uint8_t instance_id : 5; //!< Instance ID
#endif

#if defined(__LITTLE_ENDIAN_BITFIELD)
	uint8_t type : 6;	//!< PLDM type
	uint8_t header_ver : 2; //!< Header version
#elif defined(__BIG_ENDIAN_BITFIELD)
	uint8_t header_ver : 2;	 //!< Header version
	uint8_t type : 6;	 //!< PLDM type
#endif
	uint8_t command; //!< PLDM command code
} __attribute__((packed));

// Macros for byte-swapping variables in-place
#define HTOLE32(X) (X = htole32(X))
#define HTOLE16(X) (X = htole16(X))
#define LE32TOH(X) (X = le32toh(X))
#define LE16TOH(X) (X = le16toh(X))

/** @struct pldm_msg
 *
 * Structure representing PLDM message
 */
struct pldm_msg {
	struct pldm_msg_hdr hdr; //!< PLDM message header
	uint8_t payload[1]; //!< &payload[0] is the beginning of the payload
} __attribute__((packed));

/** @struct pldm_header_info
 *
 *  The information needed to prepare PLDM header and this is passed to the
 *  pack_pldm_header and unpack_pldm_header API.
 */
struct pldm_header_info {
	MessageType msg_type;	 //!< PLDM message type
	uint8_t instance;	 //!< PLDM instance id
	uint8_t pldm_type;	 //!< PLDM type
	uint8_t command;	 //!< PLDM command code
	uint8_t completion_code; //!< PLDM completion code, applies for response
};

/** @struct pldm_cc_only_rsp
 *
 *  Structure representing generic pldm response message with only
 *  completion code
 */
struct pldm_cc_only_rsp {
	uint8_t completion_code; //!< PLDM completion code
} __attribute__((packed));

/** @struct pldm_get_types_resp
 *
 *  Structure representing PLDM get types response.
 */
struct pldm_get_types_resp {
	uint8_t completion_code; //!< completion code
	// Each bit represents whether a given PLDM type is supported
	bitfield8_t types[PLDM_MAX_TYPES / 8];
} __attribute__((packed));

/** @struct pldm_get_commands_req
 *
 *  Structure representing PLDM get commands request.
 */
struct pldm_get_commands_req {
	uint8_t type;	 //!< PLDM Type for which command support information is
			 //!< being requested
	ver32_t version; //!< version for the specified PLDM Type
} __attribute__((packed));

/** @struct pldm_get_commands_resp
 *
 *  Structure representing PLDM get commands response.
 */
struct pldm_get_commands_resp {
	uint8_t completion_code;  //!< completion code
	bitfield8_t commands[32]; //!< each bit represents whether a given PLDM
				  //!< command is supported
} __attribute__((packed));

/** @struct pldm_get_version_req
 *
 *  Structure representing PLDM get version request.
 */
struct pldm_get_version_req {
	uint32_t
	    transfer_handle; //!< handle to identify PLDM version data transfer
	uint8_t transfer_opflag; //!< PLDM GetVersion operation flag
	uint8_t type; //!< PLDM Type for which version information is being
		      //!< requested
} __attribute__((packed));

/** @struct pldm_set_tid_req
 *
 *  Structure representing PLDM set terminus id request.
 */
struct pldm_set_tid_req {
	uint8_t tid; //!< Terminus ID to be set
} __attribute__((packed));

/** @struct pldm_set_tid_rsp
 *
 *  Structure representing PLDM set terminus id response.
 */
typedef struct pldm_cc_only_rsp pldm_set_tid_rsp;

/** @struct pldm_get_version_resp
 *
 *  Structure representing PLDM get version response.
 */

struct pldm_get_version_resp {
	uint8_t completion_code;       //!< completion code
	uint32_t next_transfer_handle; //!< next portion of PLDM version data
				       //!< transfer
	uint8_t transfer_flag;	       //!< PLDM GetVersion transfer flag
	uint8_t version_data[1];       //!< PLDM GetVersion version field
} __attribute__((packed));

/** @struct pldm_get_tid_resp
 *
 *  Structure representing PLDM get tid response.
 */

struct pldm_get_tid_resp {
	uint8_t completion_code; //!< completion code
	uint8_t tid;		 //!< PLDM GetTID TID field
} __attribute__((packed));

/**
 * @brief Populate the PLDM message with the PLDM header.The caller of this API
 *        allocates buffer for the PLDM header when forming the PLDM message.
 *        The buffer is passed to this API to pack the PLDM header.
 *
 * @param[in] hdr - Pointer to the PLDM header information
 * @param[out] msg - Pointer to PLDM message header
 *
 * @return 0 on success, otherwise PLDM error codes.
 * @note   Caller is responsible for alloc and dealloc of msg
 *         and hdr params
 */
int pack_pldm_header(const struct pldm_header_info *hdr,
		     struct pldm_msg_hdr *msg);

/**
 * @brief Unpack the PLDM header from the PLDM message.
 *
 * @param[in] msg - Pointer to the PLDM message header
 * @param[out] hdr - Pointer to the PLDM header information
 *
 * @return 0 on success, otherwise PLDM error codes.
 * @note   Caller is responsible for alloc and dealloc of msg
 *         and hdr params
 */
int unpack_pldm_header(const struct pldm_msg_hdr *msg,
		       struct pldm_header_info *hdr);

/* Requester */

/* GetPLDMTypes */

/** @brief Create a PLDM request message for GetPLDMTypes
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_types_req(uint8_t instance_id, struct pldm_msg *msg);

/** @brief Decode a GetPLDMTypes response message
 *
 *  Note:
 *  * If the return value is not PLDM_SUCCESS, it represents a
 * transport layer error.
 *  * If the completion_code value is not PLDM_SUCCESS, it represents a
 * protocol layer error and all the out-parameters are invalid.
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of response message payload
 *  @param[out] completion_code - Pointer to response msg's PLDM completion code
 *  @param[out] types - pointer to array bitfield8_t[8] containing supported
 *              types (MAX_TYPES/8) = 8), as per DSP0240
 *  @return pldm_completion_codes
 */
int decode_get_types_resp(const struct pldm_msg *msg, size_t payload_length,
			  uint8_t *completion_code, bitfield8_t *types);

/* GetPLDMCommands */

/** @brief Create a PLDM request message for GetPLDMCommands
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] type - PLDM Type
 *  @param[in] version - Version for PLDM Type
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_commands_req(uint8_t instance_id, uint8_t type, ver32_t version,
			    struct pldm_msg *msg);

/** @brief Decode a GetPLDMCommands response message
 *
 *  Note:
 *  * If the return value is not PLDM_SUCCESS, it represents a
 * transport layer error.
 *  * If the completion_code value is not PLDM_SUCCESS, it represents a
 * protocol layer error and all the out-parameters are invalid.
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of reponse message payload
 *  @param[out] completion_code - Pointer to response msg's PLDM completion code
 *  @param[in] commands - pointer to array bitfield8_t[32] containing supported
 *             commands (PLDM_MAX_CMDS_PER_TYPE/8) = 32), as per DSP0240
 *  @return pldm_completion_codes
 */
int decode_get_commands_resp(const struct pldm_msg *msg, size_t payload_length,
			     uint8_t *completion_code, bitfield8_t *commands);

/* GetPLDMVersion */

/** @brief Create a PLDM request for GetPLDMVersion
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] transfer_handle - Handle to identify PLDM version data transfer.
 *         This handle is ignored by the responder when the
 *         transferop_flag is set to getFirstPart.
 *  @param[in] transfer_opflag - flag to indicate whether it is start of
 *         transfer
 *  @param[in] type -  PLDM Type for which version is requested
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_version_req(uint8_t instance_id, uint32_t transfer_handle,
			   uint8_t transfer_opflag, uint8_t type,
			   struct pldm_msg *msg);

/** @brief Decode a GetPLDMVersion response message
 *
 *  Note:
 *  * If the return value is not PLDM_SUCCESS, it represents a
 * transport layer error.
 *  * If the completion_code value is not PLDM_SUCCESS, it represents a
 * protocol layer error and all the out-parameters are invalid.
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of reponse message payload
 *  @param[out] completion_code - Pointer to response msg's PLDM completion code
 *  @param[out] next_transfer_handle - the next handle for the next part of data
 *  @param[out] transfer_flag - flag to indicate the part of data
 *  @param[out] version - variable field structure in which ptr will be pointing
 *    to version data and length will contain size of version data in bytes.
 *    length will include last 4 bytes crc field also.
 *  @return pldm_completion_codes
 *  @note data pointed by version->ptr will be a portion of msg and need not
 *   be freed
 *  @note crc integrity check is not handled here and should be handled
 *  by caller
 */
int decode_get_version_resp(const struct pldm_msg *msg,
			    const size_t payload_length,
			    uint8_t *completion_code,
			    uint32_t *next_transfer_handle,
			    uint8_t *transfer_flag,
			    struct variable_field *version);

/* GetTID */

/** @brief Decode a GetTID response message
 *
 *  Note:
 *  * If the return value is not PLDM_SUCCESS, it represents a
 * transport layer error.
 *  * If the completion_code value is not PLDM_SUCCESS, it represents a
 * protocol layer error and all the out-parameters are invalid.
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of response message payload
 *  @param[out] completion_code - Pointer to response msg's PLDM completion code
 *  @param[out] tid - Pointer to the terminus id
 *  @return pldm_completion_codes
 */
int decode_get_tid_resp(const struct pldm_msg *msg, size_t payload_length,
			uint8_t *completion_code, uint8_t *tid);

/* Responder */

/* GetPLDMTypes */

/** @brief Create a PLDM response message for GetPLDMTypes
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] completion_code - PLDM completion code
 *  @param[in] types - pointer to array bitfield8_t[8] containing supported
 *             types (MAX_TYPES/8) = 8), as per DSP0240
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_types_resp(uint8_t instance_id, uint8_t completion_code,
			  const bitfield8_t *types, struct pldm_msg *msg);

/* GetPLDMCommands */

/** @brief Decode GetPLDMCommands' request data
 *
 *  @param[in] msg - Request message
 *  @param[in] payload_length - Length of request message payload
 *  @param[out] type - PLDM Type
 *  @param[out] version - Version for PLDM Type
 *  @return pldm_completion_codes
 */
int decode_get_commands_req(const struct pldm_msg *msg, size_t payload_length,
			    uint8_t *type, ver32_t *version);

/** @brief Create a PLDM response message for GetPLDMCommands
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] completion_code - PLDM completion code
 *  @param[in] commands - pointer to array bitfield8_t[32] containing supported
 *             commands (PLDM_MAX_CMDS_PER_TYPE/8) = 32), as per DSP0240
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_commands_resp(uint8_t instance_id, uint8_t completion_code,
			     const bitfield8_t *commands, struct pldm_msg *msg);

/* GetPLDMVersion */

/** @brief Create a PLDM response for GetPLDMVersion
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] completion_code - PLDM completion code
 *  @param[in] next_transfer_handle - Handle to identify next portion of
 *              data transfer
 *  @param[in] transfer_flag - Represents the part of transfer
 *  @param[in] version_data - the version data. last 4 bytes
 *      will be crc32 of versions also including previously transfered
 *      versions if it was a multipart response
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_version_resp(const uint8_t instance_id,
			    const uint8_t completion_code,
			    const uint32_t next_transfer_handle,
			    const uint8_t transfer_flag,
			    const struct variable_field *version_data,
			    struct pldm_msg *msg);

/** @brief Decode a GetPLDMVersion request message
 *
 *  @param[in] msg - Request message
 *  @param[in] payload_length - length of request message payload
 *  @param[out] transfer_handle - the handle of data
 *  @param[out] transfer_opflag - Transfer Flag
 *  @param[out] type - PLDM type for which version is requested
 *  @return pldm_completion_codes
 */
int decode_get_version_req(const struct pldm_msg *msg,
			   const size_t payload_length,
			   uint32_t *transfer_handle, uint8_t *transfer_opflag,
			   uint8_t *type);

/* Requester */

/* GetTID */

/** @brief Create a PLDM request message for GetTID
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_tid_req(uint8_t instance_id, struct pldm_msg *msg);

/** @brief Create a PLDM response message for GetTID
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] completion_code - PLDM completion code
 *  @param[in] tid - Terminus ID
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_get_tid_resp(uint8_t instance_id, uint8_t completion_code,
			uint8_t tid, struct pldm_msg *msg);

/** @brief Create a PLDM response message containing only cc
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] type - PLDM Type
 *  @param[in] command - PLDM Command
 *  @param[in] cc - PLDM Completion Code
 *  @param[out] msg - Message will be written to this
 *  @return pldm_completion_codes
 */
int encode_cc_only_resp(uint8_t instance_id, uint8_t type, uint8_t command,
			uint8_t cc, struct pldm_msg *msg);

/** @brief Decode a PLDM response message containing only completion code
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of response message payload. Should be 1
 * byte.
 *  @param[out] completion_code - Pointer to store response msg's PLDM
 * completion code
 *  @return pldm_completion_codes
 */
int decode_cc_only_resp(const struct pldm_msg *msg, const size_t payload_length,
			uint8_t *completion_code);

/*SetTID*/

/** @brief Create a PLDM request message for SetTID
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] tid - Terminus ID to be set
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
int encode_set_tid_req(const uint8_t instance_id, const uint8_t tid,
		       struct pldm_msg *msg);

/** @brief Create a PLDM response message for SetTID
 *
 *  @param[in] instance_id - Message's instance id
 *  @param[in] completion_code - PLDM completion code
 *  @param[in,out] msg - Message will be written to this
 *  @return pldm_completion_codes
 *  @note  Caller is responsible for memory alloc and dealloc of param
 *         'msg.payload'
 */
inline int encode_set_tid_resp(const uint8_t instance_id,
			       const uint8_t completion_code,
			       struct pldm_msg *msg)
{
	return encode_cc_only_resp(instance_id, PLDM_BASE, PLDM_SET_TID,
				   completion_code, msg);
}

/** @brief Decode a SetTID request message
 *
 *  @param[in] msg - Request message
 *  @param[in] payload_length - length of request message payload
 *  @param[out] tid - Terminus ID to be set
 *  @return pldm_completion_codes
 */
int decode_set_tid_req(const struct pldm_msg *msg, const size_t payload_length,
		       uint8_t *tid);

/** @brief Decode a SetTID response message
 *
 *  @param[in] msg - Response message
 *  @param[in] payload_length - Length of response message payload
 *  @param[out] completion_code - Pointer to store response msg's PLDM
 * completion code
 *  @return pldm_completion_codes
 */
inline int decode_set_tid_resp(const struct pldm_msg *msg,
			       const size_t payload_length,
			       uint8_t *completion_code)
{
	return decode_cc_only_resp(msg, payload_length, completion_code);
}

/** @brief Create a PLDM request message contains empty payload
 *
 *	@param[in] instance_id - Message's instance id
 *	@param[in] pldm_type - PLDM Type
 *	@param[in] command - PLDM Command
 *	@param[out] msg - Message will be written to this
 *	@return pldm_completion_codes
 */
int encode_header_only_request(const uint8_t instance_id,
			       const uint8_t pldm_type, const uint8_t command,
			       struct pldm_msg *msg);

/** @brief Create a PLDM request message contains empty payload
 *
 *	@param[in] instance_id - Message's instance id
 *	@param[in] pldm_type - PLDM Type
 *	@param[in] command - PLDM Command
 *	@param[in] msg_type - PLDM message type
 *	@param[out] msg - Message will be written to this
 *	@return pldm_completion_codes
 */
int encode_pldm_header(const uint8_t instance_id, const uint8_t pldm_type,
		       const uint8_t command, const uint8_t msg_type,
		       struct pldm_msg *msg);

#ifdef __cplusplus
}
#endif

#endif /* BASE_H */
