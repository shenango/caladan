struct spdk_nvme_status {
	uint16_t p	:  1;	/* phase tag */
	uint16_t sc	:  8;	/* status code */
	uint16_t sct	:  3;	/* status code type */
	uint16_t rsvd2	:  2;
	uint16_t m	:  1;	/* more */
	uint16_t dnr	:  1;	/* do not retry */
};

struct spdk_nvme_cpl {
	uint32_t		cdw0;	/* command-specific */
	uint32_t		rsvd1;
	uint16_t		sqhd;	/* submission queue head pointer */
	uint16_t		sqid;	/* submission queue identifier */
	uint16_t		cid;	/* command identifier */
	struct spdk_nvme_status	status;
};
BUILD_ASSERT(sizeof(struct spdk_nvme_cpl) == 16);

