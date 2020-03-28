#ifndef _COMMON_TYPES_H
#define _COMMON_TYPES_H
/* VERSION */
#define VERSION				1
#define MAX_CLIENT			5
/* message type define */

#define BUFF_SIZE 			1024
#define TIME_STR_LEN_MAX 	32
#define MAX_TEST_PERIOD		(40*60) /* 40 minutes */
#define RECORD_ITEM_NUM_MAX (2000 * MAX_TEST_PERIOD) /* 2000 pps */


enum {
	DBG_LOG_ERR		= 1,
	DBG_LOG_WAR,
	DBG_LOG_DUMP,
	DBG_LOG_INFO,
	DBG_LOG_NORMAL,
	DBG_LOG_VERBOSE,
};


extern uint32_t debug_lvl;
	
#define debug_log(lvl,fmt,...) \
do{ \
	if(lvl <= debug_lvl) { \
	printf("\r\n"); \
	printf(fmt,##__VA_ARGS__); \
	printf("\r\n"); \
	}\
}while(0)



/** uses these enumerators to indicate the error codes. */
typedef enum
{
	/** Success. */
	RC_E_NONE = 0,
	/** Error in RPC. */
	RC_E_RPC                  = -20,
	/** Internal error. */
	RC_E_INTERNAL             = -21,
	/** Invalid parameter. */
	RC_E_PARAM                = -22,
	/** Parameter constraint violated. */
	RC_E_ERROR                = -23,
	/** Maximum count is already reached or table full. */
	RC_E_FULL                 = -24,
	/** Already exists. */
	RC_E_EXISTS               = -25,
	/** Operation Timeout. */
	RC_E_TIMEOUT              = -26,
	/** Operation Fail. */
	RC_E_FAIL                 = -27,
	/** Disabled. */
	RC_E_DISABLED             = -28,
	/** Parameter/feature is not supported. */
	RC_E_UNAVAIL              = -29,
	/** Parameter not found. */
	RC_E_NOT_FOUND            = -30,
	/** Nothing to report or table is empty. */
	RC_E_EMPTY                = -31,
	/** Request denied. */
	RC_E_REQUEST_DENIED       = -32,
	/** Not implemented. */
	RC_NOT_IMPLEMENTED_YET    = -33,

	/* Pkt need be dropped */
	RC_E_DROP 	 							 = -34,

	/* Pkt unsupported format */
	RC_E_FORMAT							 = -35,
} RC_ERROR_t;


enum __MSG_TYPE__
{
	MSG_TYPE_CFG_REQUEST	= 1,
	MSG_TYPE_CFG_REPLY	,
	MSG_TYPE_CFG_DBG	,
	MSG_TYPE_NTP_REQUEST,
	MSG_TYPE_NTP_REPLY	,
	
	MSG_TYPE_STATUS_REQUEST = 100	,
	MSG_TYPE_STATUS_REPLY 	,
	MSG_TYPE_TEST_STATUS_REPLY	,

	
	MSG_TYPE_RAW_DATA  		= 200	,
	MSG_TYPE_STD_DATA 	,
	MSG_TYPE_YPR_DATA 	,
	MSG_TYPE_EXT_RAW_DATA 	,
	MSG_TYPE_EXT_RAW_DATA_32 	,
	

	MSG_TYPE_RECORD_REQUEST = 210	,
	MSG_TYPE_RECORD_REPLY	,
	MSG_TYPE_RECORD_ITEM_REPLY	,


};

enum __CFG_CMD__
{
	MSG_CFG_SAMPLE_START        = 1,
	MSG_CMD_SAMPLE_STOP,
	MSG_CMD_DATA_MODE,
};


enum __SAMPLE_MODE__
{
	SAMPLE_MODE_SINGLE        = 1,
	SAMPLE_MODE_CONTINUE,
};


typedef enum{
	MSG_RECORD_LIST = 1,
	MSG_RECORD_ITEM = 2,

}MSG_RECORD_TYPE_e;




typedef struct MSG_HDR_S
{
	uint8_t ver;
	uint8_t type;
	uint16_t length;
	uint32_t xid;
}MSG_HDR_t;

typedef struct MSG_CFG_S
{
	MSG_HDR_t hdr;
	uint8_t data[0];

}MSG_CFG_t;

typedef struct MSG_CFG_REQUEST_S
{
	MSG_HDR_t 	hdr;
	uint32_t 	cmd;
	uint8_t 	name[64];
	uint32_t 	mode;
	uint32_t 	period;
	uint32_t 	sample_rate;
	uint32_t 	fir_ctrl; 
	uint32_t 	backhaul;
	
}MSG_CFG_REQUEST_t;


typedef struct MSG_CFG_REPLY_S
{
	MSG_HDR_t hdr;
	int32_t result;
	uint32_t flag;
}MSG_CFG_REPLY_t;

typedef struct MSG_STATUS_REQUEST_S
{
	MSG_HDR_t hdr;
	uint32_t cmd;
	uint32_t param;
}MSG_STATUS_REQUEST_t;

typedef struct MSG_STATUS_REPLY_S
{
	MSG_HDR_t hdr;
	uint32_t ts;
	float pack_voltage;
	float cell_voltage;
	float jack_voltage;
	int32_t temp;

}MSG_STATUS_REPLY_t;


typedef struct MSG_TEST_STATUS_REPLY_S
{
	MSG_HDR_t hdr;
	uint32_t ts;
	int32_t result;
	uint32_t param;

}MSG_TEST_STATUS_REPLY_t;


typedef struct MSG_RECORD_REQUEST_S
{
	MSG_HDR_t hdr;
	uint32_t cmd;
	uint32_t param;
}MSG_RECORD_REQUEST_t;

typedef struct MSG_RECORD_REQUEST_ITEM_S
{
	MSG_HDR_t hdr;
	uint32_t cmd;
	uint8_t name[0];
}MSG_RECORD_REQUEST_ITEM_t;





typedef struct MSG_RECORD_ITEM_REPLY_S
{
	MSG_HDR_t hdr;
	uint32_t crc;
	uint8_t name[64];
	int32_t frag;
	uint8_t body[0];
}MSG_RECORD_ITEM_REPLY_t;


typedef struct MSG_EXT_RAW_DATA_S
{
	MSG_HDR_t hdr;
	struct RECORD_ITEM_S *data;
}MSG_EXT_RAW_DATA_t;


struct MSG_RAW_DATA_S
{
	MSG_HDR_t hdr;
	uint32_t ts;
	int16_t gyro[3];
	int16_t accel[3];
	int32_t quat[4];
	int16_t compass[3];
	int32_t temp;
};

struct MSG_STD_DATA_S
{
	MSG_HDR_t hdr;
	uint32_t ts;
	float gyro[3];
	float accel[3];
	float compass[3];
	float temp;
};


struct MSG_YPR_DATA_S
{
	MSG_HDR_t hdr;
	uint32_t ts;
	float ypr[3];
	float compass[3];
	float temp;
};


#define JAN_1970     		0x83aa7e80
#define NTPFRAC(x) (4294 * (x) + ((1981 * (x))>>11))
#define USEC(x) (((x) >> 12) - 759 * ((((x) >> 10) + 32768) >> 16))


typedef struct NTP_TIMESTAMP_S
{
	uint32_t integer_p;
	uint32_t fraction_p;
}NTP_TIMESTAMP_t;

typedef struct MSG_NTP_REQUEST_S
{
	MSG_HDR_t hdr;
	NTP_TIMESTAMP_t c_tx_ts;
}MSG_NTP_REQUEST_t;


typedef struct MSG_NTP_REPLY_S
{
	MSG_HDR_t hdr;
	NTP_TIMESTAMP_t c_tx_ts;
	NTP_TIMESTAMP_t s_rx_ts;
	NTP_TIMESTAMP_t s_tx_ts;
}MSG_NTP_REPLY_t;


typedef struct RECORD_ITEM_S
{
	uint16_t pad;
	uint32_t gyro[3];
	uint32_t accel[3];
	uint16_t cntr;
	uint64_t ts;
}__attribute__ ((packed)) RECORD_ITEM_t ;


typedef struct RECORD_POOL_S
{
	uint32_t 		r_index;
	uint32_t 		w_index;
	uint32_t 		cnt;
	RECORD_ITEM_t	pool[RECORD_ITEM_NUM_MAX];
	struct RECORD_POOL_S   *next;
}RECORD_POOL_t;


typedef struct SENSOR_DATA_RECORD_S
{
	RECORD_POOL_t up;
	RECORD_POOL_t down;

}SENSOR_DATA_RECORD_t;


typedef void (*poll_fd_ready_callback_f)(
    int fd,
    void *cookie,
    int read_ready,
    int write_ready,
    int error_seen);

typedef struct soc_map_s {
    short fd;
    uint16_t pollfd_index;
    int priority;
    poll_fd_ready_callback_f callback;
    void *cookie;
} soc_map_t;



#endif /* _COMMON_TYPES_H */

