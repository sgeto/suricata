/*
 * Copyright (c) 2009, 2010 Open Information Security Foundation
 * app-layer-dcerpc.c
 *
 * \author Kirby Kuehl <kkuehl@gmail.com>
 */
#include "suricata-common.h"

#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-debug.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"

#include "util-binsearch.h"
#include "util-unittest.h"

#include "app-layer-dcerpc.h"

enum {
    DCERPC_FIELD_NONE = 0,
    DCERPC_PARSE_DCERPC_HEADER,
    DCERPC_PARSE_DCERPC_BIND,
    DCERPC_PARSE_DCERPC_BIND_ACK,
    DCERPC_PARSE_DCERPC_REQUEST,
    /* must be last */
    DCERPC_FIELD_MAX,
};

#if 0
/* \brief hexdump function from libdnet, used for debugging only */
void hexdump(const void *buf, size_t len) {
    /* dumps len bytes of *buf to stdout. Looks like:
     * [0000] 75 6E 6B 6E 6F 77 6E 20
     *                  30 FF 00 00 00 00 39 00 unknown 0.....9.
     * (in a single line of course)
     */

    const unsigned char *p = buf;
    unsigned char c;
    size_t n;
    char bytestr[4] = {0};
    char addrstr[10] = {0};
    char hexstr[16 * 3 + 5] = {0};
    char charstr[16 * 1 + 5] = {0};
    for (n = 1; n <= len; n++) {
        if (n % 16 == 1) {
            /* store address for this line */
#if __WORDSIZE == 64
            snprintf(addrstr, sizeof(addrstr), "%.4lx",
                    ((uint64_t)p-(uint64_t)buf) );
#else
            snprintf(addrstr, sizeof(addrstr), "%.4x", ((uint32_t) p
                        - (uint32_t) buf));
#endif
        }

        c = *p;
        if (isalnum(c) == 0) {
            c = '.';
        }

        /* store hex str (for left side) */
        snprintf(bytestr, sizeof(bytestr), "%02X ", *p);
        strncat(hexstr, bytestr, sizeof(hexstr) - strlen(hexstr) - 1);

        /* store char str (for right side) */
        snprintf(bytestr, sizeof(bytestr), "%c", c);
        strncat(charstr, bytestr, sizeof(charstr) - strlen(charstr) - 1);

        if (n % 16 == 0) {
            /* line completed */
            printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
            hexstr[0] = 0;
            charstr[0] = 0;
        } else if (n % 8 == 0) {
            /* half line: add whitespaces */
            strncat(hexstr, "  ", sizeof(hexstr) - strlen(hexstr) - 1);
            strncat(charstr, " ", sizeof(charstr) - strlen(charstr) - 1);
        }
        p++; /* next byte */
    }

    if (strlen(hexstr) > 0) {
        /* print rest of buffer if not empty */
        printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
    }
}
#endif

/**
 * \brief printUUID function used to print UUID, Major and Minor Version Number
 * and if it was Accepted or Rejected in the BIND_ACK.
 */
void printUUID(char *type, struct uuid_entry *uuid) {
    uint8_t i = 0;
    printf("%s UUID [%2u] %s ", type, uuid->ctxid,
            (uuid->result == 0) ? "Accepted" : "Rejected");
    for (i = 0; i < 16; i++) {
        printf("%02x", uuid->uuid[i]);
    }
    printf(" Major Version 0x%04x Minor Version 0x%04x\n", uuid->version,
            uuid->versionminor);
}

/**
 * \brief DCERPCParseSecondaryAddr reads secondaryaddrlen bytes from the BIND_ACK
 * DCERPC call.
 */
static uint32_t DCERPCParseSecondaryAddr(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    while (sstate->secondaryaddrlenleft-- && input_len--) {
        SCLogDebug("0x%02x ", *p);
        p++;
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t PaddingParser(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    while (sstate->padleft-- && input_len--) {
        SCLogDebug("0x%02x ", *p);
        p++;
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t DCERPCGetCTXItems(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    if (input_len) {
        switch (sstate->ctxbytesprocessed) {
            case 0:
                if (input_len >= 4) {
                    sstate->numctxitems = *p;
                    sstate->numctxitemsleft = sstate->numctxitems;
                    sstate->ctxbytesprocessed += 4;
                    sstate->bytesprocessed += 4;
                    SCReturnUInt(4U);
                } else {
                    sstate->numctxitems = *(p++);
                    sstate->numctxitemsleft = sstate->numctxitems;
                    if (!(--input_len))
                        break;
                }
            case 1:
                p++;
                if (!(--input_len))
                    break;
            case 2:
                p++;
                if (!(--input_len))
                    break;
            case 3:
                p++;
                input_len--;
                break;
        }
    }
    sstate->ctxbytesprocessed += (p - input);
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

/**
 * \brief DCERPCParseBINDCTXItem is called for each CTXItem found the DCERPC BIND call.
 * each UUID is added to a TAILQ.
 */

static uint32_t DCERPCParseBINDCTXItem(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;

    if (input_len) {
        switch (sstate->ctxbytesprocessed) {
            case 0:
                if (input_len >= 44) {
                    sstate->ctxid = *(p);
                    sstate->ctxid |= *(p + 1) << 8;
                    sstate->uuid[3] = *(p + 4);
                    sstate->uuid[2] = *(p + 5);
                    sstate->uuid[1] = *(p + 6);
                    sstate->uuid[0] = *(p + 7);
                    sstate->uuid[5] = *(p + 8);
                    sstate->uuid[4] = *(p + 9);
                    sstate->uuid[7] = *(p + 10);
                    sstate->uuid[6] = *(p + 11);
                    sstate->uuid[8] = *(p + 12);
                    sstate->uuid[9] = *(p + 13);
                    sstate->uuid[10] = *(p + 14);
                    sstate->uuid[11] = *(p + 15);
                    sstate->uuid[12] = *(p + 16);
                    sstate->uuid[13] = *(p + 17);
                    sstate->uuid[14] = *(p + 18);
                    sstate->uuid[15] = *(p + 19);
                    sstate->version = *(p + 20);
                    sstate->version |= *(p + 21) << 8;
                    sstate->versionminor = *(p + 22);
                    sstate->versionminor |= *(p + 23) << 8;
                    sstate->uuid_entry = (struct uuid_entry *) calloc(1,
                            sizeof(struct uuid_entry));
                    if (sstate->uuid_entry == NULL) {
                        SCReturnUInt(0);
                    } else {
                        memcpy(sstate->uuid_entry->uuid, sstate->uuid,
                                sizeof(sstate->uuid));
                        sstate->uuid_entry->ctxid = sstate->ctxid;
                        sstate->uuid_entry->version = sstate->version;
                        sstate->uuid_entry->versionminor = sstate->versionminor;
                        TAILQ_INSERT_HEAD(&sstate->uuid_list, sstate->uuid_entry,
                                next);
                        //printUUID("BIND", sstate->uuid_entry);
                    }
                    sstate->numctxitemsleft--;
                    sstate->bytesprocessed += (44);
                    sstate->ctxbytesprocessed += (44);
                    SCReturnUInt(44U);
                } else {
                    sstate->ctxid = *(p++);
                    if (!(--input_len))
                        break;
                }
            case 1:
                sstate->ctxid |= *(p++) << 8;
                if (!(--input_len))
                    break;
            case 2:
                /* num transact items */
                p++;
                if (!(--input_len))
                    break;
            case 3:
                /* reserved */
                p++;
                if (!(--input_len))
                    break;
            case 4:
                sstate->uuid[3] = *(p++);
                if (!(--input_len))
                    break;
            case 5:
                sstate->uuid[2] = *(p++);
                if (!(--input_len))
                    break;
            case 6:
                sstate->uuid[1] = *(p++);
                if (!(--input_len))
                    break;
            case 7:
                sstate->uuid[0] = *(p++);
                if (!(--input_len))
                    break;
            case 8:
                sstate->uuid[5] = *(p++);
                if (!(--input_len))
                    break;
            case 9:
                sstate->uuid[4] = *(p++);
                if (!(--input_len))
                    break;
            case 10:
                sstate->uuid[7] = *(p++);
                if (!(--input_len))
                    break;
            case 11:
                sstate->uuid[6] = *(p++);
                if (!(--input_len))
                    break;
            case 12:
                sstate->uuid[8] = *(p++);
                if (!(--input_len))
                    break;
            case 13:
                sstate->uuid[9] = *(p++);
                if (!(--input_len))
                    break;
            case 14:
                sstate->uuid[10] = *(p++);
                if (!(--input_len))
                    break;
            case 15:
                sstate->uuid[11] = *(p++);
                if (!(--input_len))
                    break;
            case 16:
                sstate->uuid[12] = *(p++);
                if (!(--input_len))
                    break;
            case 17:
                sstate->uuid[13] = *(p++);
                if (!(--input_len))
                    break;
            case 18:
                sstate->uuid[14] = *(p++);
                if (!(--input_len))
                    break;
            case 19:
                sstate->uuid[15] = *(p++);
                if (!(--input_len))
                    break;
            case 20:
                sstate->version = *(p++);
                if (!(--input_len))
                    break;
            case 21:
                sstate->version |= *(p++);
                if (!(--input_len))
                    break;
            case 22:
                sstate->versionminor = *(p++);
                if (!(--input_len))
                    break;
            case 23:
                sstate->versionminor |= *(p++);
                if (!(--input_len))
                    break;
            case 24:
                p++;
                if (!(--input_len))
                    break;
            case 25:
                p++;
                if (!(--input_len))
                    break;
            case 26:
                p++;
                if (!(--input_len))
                    break;
            case 27:
                p++;
                if (!(--input_len))
                    break;
            case 28:
                p++;
                if (!(--input_len))
                    break;
            case 29:
                p++;
                if (!(--input_len))
                    break;
            case 30:
                p++;
                if (!(--input_len))
                    break;
            case 31:
                p++;
                if (!(--input_len))
                    break;
            case 32:
                p++;
                if (!(--input_len))
                    break;
            case 33:
                p++;
                if (!(--input_len))
                    break;
            case 34:
                p++;
                if (!(--input_len))
                    break;
            case 35:
                p++;
                if (!(--input_len))
                    break;
            case 36:
                p++;
                if (!(--input_len))
                    break;
            case 37:
                p++;
                if (!(--input_len))
                    break;
            case 38:
                p++;
                if (!(--input_len))
                    break;
            case 39:
                p++;
                if (!(--input_len))
                    break;
            case 40:
                p++;
                if (!(--input_len))
                    break;
            case 41:
                p++;
                if (!(--input_len))
                    break;
            case 42:
                p++;
                if (!(--input_len))
                    break;
            case 43:
                sstate->numctxitemsleft--;
                if (sstate->uuid_entry == NULL) {
                    SCReturnUInt(0);
                } else {
                    memcpy(sstate->uuid_entry->uuid, sstate->uuid,
                            sizeof(sstate->uuid));
                    sstate->uuid_entry->ctxid = sstate->ctxid;
                    sstate->uuid_entry->version = sstate->version;
                    sstate->uuid_entry->versionminor = sstate->versionminor;
                    TAILQ_INSERT_HEAD(&sstate->uuid_list, sstate->uuid_entry, next);
                }
                p++;
                --input_len;
                break;
        }
    }
    sstate->ctxbytesprocessed += (p - input);
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

/**
 * \brief DCERPCParseBINDACKCTXItem is called for each CTXItem found in
 * the BIND_ACK call. The result (Accepted or Rejected) is added to the
 * correct UUID from the BIND call.
 */
static uint32_t DCERPCParseBINDACKCTXItem(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    struct uuid_entry *uuid_entry;

    if (input_len) {
        switch (sstate->ctxbytesprocessed) {
            case 0:
                if (input_len >= 24) {
                    sstate->result = *p;
                    sstate->result |= *(p + 1) << 8;
                    TAILQ_FOREACH(uuid_entry, &sstate->uuid_list, next) {
                        if (uuid_entry->ctxid == sstate->numctxitems
                                - sstate->numctxitemsleft) {
                            uuid_entry->result = sstate->result;
                            //printUUID("BIND_ACK", uuid_entry);
                            break;
                        }
                    }
                    sstate->numctxitemsleft--;
                    sstate->bytesprocessed += (24);
                    sstate->ctxbytesprocessed += (24);
                    SCReturnUInt(24U);
                } else {
                    sstate->result = *(p++);
                    if (!(--input_len))
                        break;
                }
            case 1:
                sstate->result |= *(p++) << 8;
                if (!(--input_len))
                    break;
            case 2:
                /* num transact items */
                p++;
                if (!(--input_len))
                    break;
            case 3:
                /* reserved */
                p++;
                if (!(--input_len))
                    break;
            case 4:
                p++;
                if (!(--input_len))
                    break;
            case 5:
                p++;
                if (!(--input_len))
                    break;
            case 6:
                p++;
                if (!(--input_len))
                    break;
            case 7:
                p++;
                if (!(--input_len))
                    break;
            case 8:
                p++;
                if (!(--input_len))
                    break;
            case 9:
                p++;
                if (!(--input_len))
                    break;
            case 10:
                p++;
                if (!(--input_len))
                    break;
            case 11:
                p++;
                if (!(--input_len))
                    break;
            case 12:
                p++;
                if (!(--input_len))
                    break;
            case 13:
                p++;
                if (!(--input_len))
                    break;
            case 14:
                p++;
                if (!(--input_len))
                    break;
            case 15:
                p++;
                if (!(--input_len))
                    break;
            case 16:
                p++;
                if (!(--input_len))
                    break;
            case 17:
                p++;
                if (!(--input_len))
                    break;
            case 18:
                p++;
                if (!(--input_len))
                    break;
            case 19:
                p++;
                if (!(--input_len))
                    break;
            case 20:
                p++;
                if (!(--input_len))
                    break;
            case 21:
                p++;
                if (!(--input_len))
                    break;
            case 22:
                p++;
                if (!(--input_len))
                    break;
            case 23:
                TAILQ_FOREACH(uuid_entry, &sstate->uuid_list, next) {
                    if (uuid_entry->ctxid == sstate->numctxitems
                            - sstate->numctxitemsleft) {
                        uuid_entry->result = sstate->result;
                        //printUUID("BIND_ACK", uuid_entry);
                        break;
                    }
                }
                sstate->numctxitemsleft--;
                p++;
                --input_len;
                break;

        }
    }
    sstate->ctxbytesprocessed += (p - input);
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t DCERPCParseBIND(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    if (input_len) {
        switch (sstate->bytesprocessed) {
            case 16:
                sstate->numctxitems = 0;
                if (input_len >= 12) {
                    TAILQ_INIT(&sstate->uuid_list);
                    sstate->numctxitems = *(p + 8);
                    sstate->numctxitemsleft = sstate->numctxitems;
                    sstate->bytesprocessed += 12;
                    SCReturnUInt(12U);
                } else {
                    /* max_xmit_frag */
                    p++;
                    if (!(--input_len))
                        break;
                }
            case 17:
                /* max_xmit_frag */
                p++;
                if (!(--input_len))
                    break;
            case 18:
                /* max_recv_frag */
                p++;
                if (!(--input_len))
                    break;
            case 19:
                /* max_recv_frag */
                p++;
                if (!(--input_len))
                    break;
            case 20:
                /* assoc_group_id */
                p++;
                if (!(--input_len))
                    break;
            case 21:
                /* assoc_group_id */
                p++;
                if (!(--input_len))
                    break;
            case 22:
                /* assoc_group_id */
                p++;
                if (!(--input_len))
                    break;
            case 23:
                /* assoc_group_id */
                p++;
                if (!(--input_len))
                    break;
            case 24:
                sstate->numctxitems = *(p++);
                sstate->numctxitemsleft = sstate->numctxitems;
                TAILQ_INIT(&sstate->uuid_list);
                if (!(--input_len))
                    break;
            case 25:
                /* pad byte 1 */
                p++;
                if (!(--input_len))
                    break;
            case 26:
                /* pad byte 2 */
                p++;
                if (!(--input_len))
                    break;
            case 27:
                /* pad byte 3 */
                p++;
                --input_len;
                break;
        }
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t DCERPCParseBINDACK(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;

    switch (sstate->bytesprocessed) {
        case 16:
            sstate->numctxitems = 0;
            if (input_len >= 10) {
                if (sstate->dcerpc.packed_drep[0] == 0x10) {
                    sstate->secondaryaddrlen = *(p + 8);
                    sstate->secondaryaddrlen |= *(p + 9) << 8;
                } else {
                    sstate->secondaryaddrlen = *(p + 8) << 8;
                    sstate->secondaryaddrlen |= *(p + 9);
                }
                sstate->secondaryaddrlenleft = sstate->secondaryaddrlen;
                sstate->bytesprocessed += 10;
                SCReturnUInt(10U);
            } else {
                /* max_xmit_frag */
                p++;
                if (!(--input_len))
                    break;
            }
        case 17:
            /* max_xmit_frag */
            p++;
            if (!(--input_len))
                break;
        case 18:
            /* max_recv_frag */
            p++;
            if (!(--input_len))
                break;
        case 19:
            /* max_recv_frag */
            p++;
            if (!(--input_len))
                break;
        case 20:
            /* assoc_group_id */
            p++;
            if (!(--input_len))
                break;
        case 21:
            /* assoc_group_id */
            p++;
            if (!(--input_len))
                break;
        case 22:
            /* assoc_group_id */
            p++;
            if (!(--input_len))
                break;
        case 23:
            /* assoc_group_id */
            p++;
            if (!(--input_len))
                break;
        case 24:
            sstate->secondaryaddrlen = *(p++);
            if (!(--input_len))
                break;
        case 25:
            sstate->secondaryaddrlen |= *(p++) << 8;
            if (sstate->dcerpc.packed_drep[0] == 0x01) {
                bswap_16(sstate->secondaryaddrlen);
            }
            sstate->secondaryaddrlenleft = sstate->secondaryaddrlen;
            SCLogDebug("secondaryaddrlen %u 0x%04x\n", sstate->secondaryaddrlen,
                    sstate->secondaryaddrlen);
            --input_len;
            break;
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t DCERPCParseREQUEST(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;

    switch (sstate->bytesprocessed) {
        case 16:
            sstate->numctxitems = 0;
            if (input_len >= 8) {
                if (sstate->dcerpc.packed_drep[0] == 0x10) {
                    sstate->opnum = *(p + 6);
                    sstate->opnum |= *(p + 7) << 8;
                } else {
                    sstate->opnum = *(p + 6) << 8;
                    sstate->opnum |= *(p + 7);
                }
                sstate->bytesprocessed += 8;
                SCReturnUInt(8U);
            } else {
                /* alloc hint 1 */
                p++;
                if (!(--input_len))
                    break;
            }
        case 17:
            /* alloc hint 2 */
            p++;
            if (!(--input_len))
                break;
        case 18:
            /* alloc hint 3 */
            p++;
            if (!(--input_len))
                break;
        case 19:
            /* alloc hint 4 */
            p++;
            if (!(--input_len))
                break;
        case 20:
            /* context id 1 */
            p++;
            if (!(--input_len))
                break;
        case 21:
            /* context id 2 */
            p++;
            if (!(--input_len))
                break;
        case 22:
            sstate->opnum = *(p++);
            if (!(--input_len))
                break;
        case 23:
            sstate->opnum |= *(p++) << 8;
            if (sstate->dcerpc.packed_drep[0] == 0x01) {
                bswap_16(sstate->opnum);
            }
            --input_len;
            break;
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static uint32_t StubDataParser(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    while (sstate->padleft-- && input_len--) {
        SCLogDebug("0x%02x ", *p);
        p++;
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

/**
 * \brief DCERPCParseHeader parses the 16 byte DCERPC header
 * A fast path for normal decoding is used when there is enough bytes
 * present to parse the entire header. A slow path is used to parse
 * fragmented packets.
 */
static uint32_t DCERPCParseHeader(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();
    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint8_t *p = input;
    if (input_len) {
        switch (sstate->bytesprocessed) {
            case 0:
                if (input_len >= DCERPC_HDR_LEN) {
                    //if (*p != 5) SCReturnUInt();
                    //if (!(*(p + 1 ) == 0 || (*(p + 1) == 1))) SCReturnInt(0);
                    sstate->dcerpc.rpc_vers = *p;
                    sstate->dcerpc.rpc_vers_minor = *(p + 1);
                    sstate->dcerpc.type = *(p + 2);
                    sstate->dcerpc.pfc_flags = *(p + 3);
                    sstate->dcerpc.packed_drep[0] = *(p + 4);
                    sstate->dcerpc.packed_drep[1] = *(p + 5);
                    sstate->dcerpc.packed_drep[2] = *(p + 6);
                    sstate->dcerpc.packed_drep[3] = *(p + 7);
                    if (sstate->dcerpc.packed_drep[0] == 0x10) {
                        sstate->dcerpc.frag_length = *(p + 8);
                        sstate->dcerpc.frag_length |= *(p + 9) << 8;
                        sstate->dcerpc.auth_length = *(p + 10);
                        sstate->dcerpc.auth_length |= *(p + 11) << 8;
                        sstate->dcerpc.call_id = *(p + 12) << 24;
                        sstate->dcerpc.call_id |= *(p + 13) << 16;
                        sstate->dcerpc.call_id |= *(p + 14) << 8;
                        sstate->dcerpc.call_id |= *(p + 15);
                    } else {
                        sstate->dcerpc.frag_length = *(p + 8) << 8;
                        sstate->dcerpc.frag_length |= *(p + 9);
                        sstate->dcerpc.auth_length = *(p + 10) << 8;
                        sstate->dcerpc.auth_length |= *(p + 11);
                        sstate->dcerpc.call_id = *(p + 12);
                        sstate->dcerpc.call_id |= *(p + 13) << 8;
                        sstate->dcerpc.call_id |= *(p + 14) << 16;
                        sstate->dcerpc.call_id |= *(p + 15) << 24;
                    }
                    sstate->bytesprocessed = DCERPC_HDR_LEN;
                    SCReturnUInt(16U);
                    break;
                } else {
                    sstate->dcerpc.rpc_vers = *(p++);
                    // if (sstate->dcerpc.rpc_vers != 5) SCReturnInt(2);
                    if (!(--input_len))
                        break;
                }
            case 1:
                sstate->dcerpc.rpc_vers_minor = *(p++);
                // if ((sstate->dcerpc.rpc_vers_minor != 0) ||
                //         (sstate->dcerpc.rpc_vers_minor != 1)) SCReturnInt(3);
                if (!(--input_len))
                    break;
            case 2:
                sstate->dcerpc.type = *(p++);
                if (!(--input_len))
                    break;
            case 3:
                sstate->dcerpc.pfc_flags = *(p++);
                if (!(--input_len))
                    break;
            case 4:
                sstate->dcerpc.packed_drep[0] = *(p++);
                if (!(--input_len))
                    break;
            case 5:
                sstate->dcerpc.packed_drep[1] = *(p++);
                if (!(--input_len))
                    break;
            case 6:
                sstate->dcerpc.packed_drep[2] = *(p++);
                if (!(--input_len))
                    break;
            case 7:
                sstate->dcerpc.packed_drep[3] = *(p++);
                if (!(--input_len))
                    break;
            case 8:
                sstate->dcerpc.frag_length = *(p++) << 8;
                if (!(--input_len))
                    break;
            case 9:
                sstate->dcerpc.frag_length |= *(p++);
                if (!(--input_len))
                    break;
            case 10:
                sstate->dcerpc.auth_length = *(p++) << 8;
                if (!(--input_len))
                    break;
            case 11:
                sstate->dcerpc.auth_length |= *(p++);
                if (!(--input_len))
                    break;
            case 12:
                sstate->dcerpc.call_id = *(p++) << 24;
                if (!(--input_len))
                    break;
            case 13:
                sstate->dcerpc.call_id |= *(p++) << 16;
                if (!(--input_len))
                    break;
            case 14:
                sstate->dcerpc.call_id |= *(p++) << 8;
                if (!(--input_len))
                    break;
            case 15:
                sstate->dcerpc.call_id |= *(p++);
                if (sstate->dcerpc.packed_drep[0] == 0x01) {
                    bswap_16(sstate->dcerpc.frag_length);
                    bswap_16(sstate->dcerpc.auth_length);
                    bswap_32(sstate->dcerpc.call_id);
                }
                --input_len;
                break;
        }
    }
    sstate->bytesprocessed += (p - input);
    SCReturnUInt((uint32_t)(p - input));
}

static int DCERPCParse(Flow *f, void *dcerpc_state,
        AppLayerParserState *pstate, uint8_t *input, uint32_t input_len,
        AppLayerParserResult *output) {
    SCEnter();

    DCERPCState *sstate = (DCERPCState *) dcerpc_state;
    uint32_t retval = 0;
    uint32_t parsed = 0;

    if (pstate == NULL)
        SCReturnInt(-1);
    while (sstate->bytesprocessed < DCERPC_HDR_LEN && input_len) {
        retval = DCERPCParseHeader(f, dcerpc_state, pstate, input, input_len,
                output);
        parsed += retval;
        input_len -= retval;
    }
    SCLogDebug("Done with DCERPCParseHeader bytesprocessed %u/%u left %u\n",
            sstate->bytesprocessed, sstate->dcerpc.frag_length, input_len);

    switch (sstate->dcerpc.type) {
        case BIND:
        case ALTER_CONTEXT:
            while (sstate->bytesprocessed < DCERPC_HDR_LEN + 12
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length
                    && input_len) {
                retval = DCERPCParseBIND(f, dcerpc_state, pstate, input + parsed,
                        input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                } else if (input_len) {
                    SCLogDebug("Error Parsing DCERPC BIND");
                    parsed -= input_len;
                    input_len = 0;
                }
            }
            SCLogDebug(
                    "Done with DCERPCParseBIND bytesprocessed %u/%u -- Should be 12\n",
                    sstate->bytesprocessed, sstate->dcerpc.frag_length);

            while (sstate->numctxitemsleft && sstate->bytesprocessed
                    < sstate->dcerpc.frag_length && input_len) {
                retval = DCERPCParseBINDCTXItem(f, dcerpc_state, pstate, input
                        + parsed, input_len, output);
                if (retval) {
                    if (sstate->ctxbytesprocessed == 44) {
                        sstate->ctxbytesprocessed = 0;
                    }
                    parsed += retval;
                    input_len -= retval;
                    SCLogDebug("BIND processed %u/%u\n", sstate->bytesprocessed,
                            sstate->dcerpc.frag_length);
                } else if (input_len) {
                    SCLogDebug("Error Parsing CTX Item");
                    parsed -= input_len;
                    input_len = 0;
                    sstate->numctxitemsleft = 0;
                }
            }
            if (sstate->bytesprocessed == sstate->dcerpc.frag_length) {
                sstate->bytesprocessed = 0;
                sstate->ctxbytesprocessed = 0;
            }
            break;
        case BIND_ACK:
        case ALTER_CONTEXT_RESP:
            while (sstate->bytesprocessed < DCERPC_HDR_LEN + 9
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length
                    && input_len) {
                retval = DCERPCParseBINDACK(f, dcerpc_state, pstate,
                        input + parsed, input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                    SCLogDebug("DCERPCParseBINDACK processed %u/%u left %u\n",
                            sstate->bytesprocessed, sstate->dcerpc.frag_length, input_len);
                } else if (input_len) {
                    SCLogDebug("Error parsing BIND_ACK");
                    parsed -= input_len;
                    input_len = 0;
                }
            }

            while (sstate->bytesprocessed < DCERPC_HDR_LEN + 10
                    + sstate->secondaryaddrlen && input_len
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length) {
                retval = DCERPCParseSecondaryAddr(f, dcerpc_state, pstate, input
                        + parsed, input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                    SCLogDebug(
                            "DCERPCParseSecondaryAddr %u/%u left %u secondaryaddr len(%u)\n",
                            sstate->bytesprocessed, sstate->dcerpc.frag_length, input_len,
                            sstate->secondaryaddrlen);
                } else if (input_len) {
                    SCLogDebug("Error parsing Secondary Address");
                    parsed -= input_len;
                    input_len = 0;
                }
            }

            if (sstate->bytesprocessed == DCERPC_HDR_LEN + 10
                    + sstate->secondaryaddrlen) {
                sstate->pad = sstate->bytesprocessed % 4;
                sstate->padleft = sstate->pad;
            }

            while (sstate->bytesprocessed < DCERPC_HDR_LEN + 10
                    + sstate->secondaryaddrlen + sstate->pad && input_len
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length) {
                retval = PaddingParser(f, dcerpc_state, pstate, input + parsed,
                        input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                    SCLogDebug("PaddingParser %u/%u left %u pad(%u)\n",
                            sstate->bytesprocessed, sstate->dcerpc.frag_length, input_len,
                            sstate->pad);
                } else if (input_len) {
                    SCLogDebug("Error parsing DCERPC Padding");
                    parsed -= input_len;
                    input_len = 0;
                }
            }

            while (sstate->bytesprocessed >= DCERPC_HDR_LEN + 10 + sstate->pad
                    + sstate->secondaryaddrlen && sstate->bytesprocessed
                    < DCERPC_HDR_LEN + 14 + sstate->pad + sstate->secondaryaddrlen
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length) {
                retval = DCERPCGetCTXItems(f, dcerpc_state, pstate, input + parsed,
                        input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                    SCLogDebug("DCERPCGetCTXItems %u/%u (%u)\n", sstate->bytesprocessed,
                            sstate->dcerpc.frag_length, sstate->numctxitems);
                } else if (input_len) {
                    SCLogDebug("Error parsing CTX Items");
                    parsed -= input_len;
                    input_len = 0;
                }
            }

            if (sstate->bytesprocessed == DCERPC_HDR_LEN + 14 + sstate->pad
                    + sstate->secondaryaddrlen) {
                sstate->ctxbytesprocessed = 0;
            }

            while (sstate->numctxitemsleft && input_len && sstate->bytesprocessed
                    < sstate->dcerpc.frag_length) {
                retval = DCERPCParseBINDACKCTXItem(f, dcerpc_state, pstate, input
                        + parsed, input_len, output);
                if (retval) {
                    if (sstate->ctxbytesprocessed == 24) {
                        sstate->ctxbytesprocessed = 0;
                    }
                    parsed += retval;
                    input_len -= retval;
                } else if (input_len) {
                    SCLogDebug("Error parsing CTX Items");
                    parsed -= input_len;
                    input_len = 0;
                    sstate->numctxitemsleft = 0;

                }
            }
            SCLogDebug("BINDACK processed %u/%u\n", sstate->bytesprocessed,
                    sstate->dcerpc.frag_length);

            if (sstate->bytesprocessed == sstate->dcerpc.frag_length) {
                sstate->bytesprocessed = 0;
                sstate->ctxbytesprocessed = 0;
            }
            break;
        case REQUEST:
            while (sstate->bytesprocessed < DCERPC_HDR_LEN + 8
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length
                    && input_len) {
                retval = DCERPCParseREQUEST(f, dcerpc_state, pstate,
                        input + parsed, input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                } else if (input_len) {
                    SCLogDebug("Error parsing DCERPC Request");
                    parsed -= input_len;
                    input_len = 0;
                }
            }
            while (sstate->bytesprocessed >= DCERPC_HDR_LEN + 8
                    && sstate->bytesprocessed < sstate->dcerpc.frag_length
                    && input_len) {
                retval = StubDataParser(f, dcerpc_state, pstate, input + parsed,
                        input_len, output);
                if (retval) {
                    parsed += retval;
                    input_len -= retval;
                } else if (input_len) {
                    SCLogDebug("Error parsing DCERPC Stub Data");
                    parsed -= input_len;
                    input_len = 0;

                }
            }
            SCLogDebug("REQUEST processed %u/%u\n", sstate->bytesprocessed,
                    sstate->dcerpc.frag_length);
            if (sstate->bytesprocessed == sstate->dcerpc.frag_length) {
                sstate->bytesprocessed = 0;
            }
            break;
        default:
            SCLogDebug("DCERPC Type 0x%02x not implemented yet\n", sstate->dcerpc.type);
            break;
    }
    pstate->parse_field = 0;
    pstate->flags |= APP_LAYER_PARSER_DONE;

    SCReturnInt(1);
}

static void *DCERPCStateAlloc(void) {
    void *s = malloc(sizeof(DCERPCState));
    if (s == NULL)
        return NULL;

    memset(s, 0, sizeof(DCERPCState));
    return s;
}

static void DCERPCStateFree(void *s) {
    DCERPCState *sstate = (DCERPCState *) s;

    struct uuid_entry *item;

    while ((item = TAILQ_FIRST(&sstate->uuid_list))) {
        TAILQ_REMOVE(&sstate->uuid_list, item, next);
        free(item);
    }

    if (s) {
        free(s);
        s = NULL;
    }
}

void RegisterDCERPCParsers(void) {
    AppLayerRegisterProto("dcerpc", ALPROTO_DCERPC, STREAM_TOSERVER,
            DCERPCParse);
    AppLayerRegisterProto("dcerpc", ALPROTO_DCERPC, STREAM_TOCLIENT,
            DCERPCParse);
    AppLayerRegisterStateFuncs(ALPROTO_DCERPC, DCERPCStateAlloc,
            DCERPCStateFree);
}

/* UNITTESTS */
#ifdef UNITTESTS

int DCERPCParserTest01(void) {
    int result = 1;
    Flow f;
    uint8_t dcerpcbind[] = {
        0x05, 0x00,
        0x0b, 0x03, 0x10, 0x00, 0x00, 0x00, 0x3c, 0x04,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x16,
        0xd0, 0x16, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x2c, 0xd0,
        0x28, 0xda, 0x76, 0x91, 0xf6, 0x6e, 0xcb, 0x0f,
        0xbf, 0x85, 0xcd, 0x9b, 0xf6, 0x39, 0x01, 0x00,
        0x03, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x2c, 0x75, 0xce, 0x7e, 0x82, 0x3b,
        0x06, 0xac, 0x1b, 0xf0, 0xf5, 0xb7, 0xa7, 0xf7,
        0x28, 0xaf, 0x05, 0x00, 0x00, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0xe3, 0xb2,
        0x10, 0xd1, 0xd0, 0x0c, 0xcc, 0x3d, 0x2f, 0x80,
        0x20, 0x7c, 0xef, 0xe7, 0x09, 0xe0, 0x04, 0x00,
        0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00,
        0x01, 0x00, 0xde, 0x85, 0x70, 0xc4, 0x02, 0x7c,
        0x60, 0x23, 0x67, 0x0c, 0x22, 0xbf, 0x18, 0x36,
        0x79, 0x17, 0x01, 0x00, 0x02, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x41, 0x65,
        0x29, 0x51, 0xaa, 0xe7, 0x7b, 0xa8, 0xf2, 0x37,
        0x0b, 0xd0, 0x3f, 0xb3, 0x36, 0xed, 0x05, 0x00,
        0x01, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x01, 0x00, 0x14, 0x96, 0x80, 0x01, 0x2e, 0x78,
        0xfb, 0x5d, 0xb4, 0x3c, 0x14, 0xb3, 0x3d, 0xaa,
        0x02, 0xfb, 0x06, 0x00, 0x00, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x3b, 0x04,
        0x68, 0x3e, 0x63, 0xfe, 0x9f, 0xd8, 0x64, 0x55,
        0xcd, 0xe7, 0x39, 0xaf, 0x98, 0x9f, 0x03, 0x00,
        0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x01, 0x00, 0x16, 0x7a, 0x4f, 0x1b, 0xdb, 0x25,
        0x92, 0x55, 0xdd, 0xae, 0x9e, 0x5b, 0x3e, 0x93,
        0x66, 0x93, 0x04, 0x00, 0x01, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0xe8, 0xa4,
        0x8a, 0xcf, 0x95, 0x6c, 0xc7, 0x8f, 0x14, 0xcc,
        0x56, 0xfc, 0x7b, 0x5f, 0x4f, 0xe8, 0x04, 0x00,
        0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x09, 0x00,
        0x01, 0x00, 0xd8, 0xda, 0xfb, 0xbc, 0xa2, 0x55,
        0x6f, 0x5d, 0xc0, 0x2d, 0x88, 0x6f, 0x00, 0x17,
        0x52, 0x8d, 0x06, 0x00, 0x03, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x3f, 0x17,
        0x55, 0x0c, 0xf4, 0x23, 0x3c, 0xca, 0xe6, 0xa0,
        0xaa, 0xcc, 0xb5, 0xe3, 0xf9, 0xce, 0x04, 0x00,
        0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x00,
        0x01, 0x00, 0x6a, 0x28, 0x19, 0x39, 0x0c, 0xb1,
        0xd0, 0x11, 0x9b, 0xa8, 0x00, 0xc0, 0x4f, 0xd9,
        0x2e, 0xf5, 0x00, 0x00, 0x00, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x0c, 0x00, 0x01, 0x00, 0xc9, 0x9f,
        0x3e, 0x6e, 0x82, 0x0a, 0x2b, 0x28, 0x37, 0x78,
        0xe1, 0x13, 0x70, 0x05, 0x38, 0x4d, 0x01, 0x00,
        0x02, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x0d, 0x00,
        0x01, 0x00, 0x11, 0xaa, 0x4b, 0x15, 0xdf, 0xa6,
        0x86, 0x3f, 0xfb, 0xe0, 0x09, 0xb7, 0xf8, 0x56,
        0xd2, 0x3f, 0x05, 0x00, 0x00, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x0e, 0x00, 0x01, 0x00, 0xee, 0x99,
        0xc4, 0x25, 0x11, 0xe4, 0x95, 0x62, 0x29, 0xfa,
        0xfd, 0x26, 0x57, 0x02, 0xf1, 0xce, 0x03, 0x00,
        0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x00,
        0x01, 0x00, 0xba, 0x81, 0x9e, 0x1a, 0xdf, 0x2b,
        0xba, 0xe4, 0xd3, 0x17, 0x41, 0x60, 0x6d, 0x2d,
        0x9e, 0x28, 0x03, 0x00, 0x03, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x10, 0x00, 0x01, 0x00, 0xa0, 0x24,
        0x03, 0x9a, 0xa9, 0x99, 0xfb, 0xbe, 0x49, 0x11,
        0xad, 0x77, 0x30, 0xaa, 0xbc, 0xb6, 0x02, 0x00,
        0x03, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x11, 0x00,
        0x01, 0x00, 0x32, 0x04, 0x7e, 0xae, 0xec, 0x28,
        0xd1, 0x55, 0x83, 0x4e, 0xc3, 0x47, 0x5d, 0x1d,
        0xc6, 0x65, 0x02, 0x00, 0x03, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x12, 0x00, 0x01, 0x00, 0xc6, 0xa4,
        0x81, 0x48, 0x66, 0x2a, 0x74, 0x7d, 0x56, 0x6e,
        0xc5, 0x1d, 0x19, 0xf2, 0xb5, 0xb6, 0x03, 0x00,
        0x02, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x13, 0x00,
        0x01, 0x00, 0xcb, 0xae, 0xb3, 0xc0, 0x0c, 0xf4,
        0xa4, 0x5e, 0x91, 0x72, 0xdd, 0x53, 0x24, 0x70,
        0x89, 0x02, 0x05, 0x00, 0x03, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x14, 0x00, 0x01, 0x00, 0xb8, 0xd0,
        0xa0, 0x1a, 0x5e, 0x7a, 0x2d, 0xfe, 0x35, 0xc6,
        0x7d, 0x08, 0x0d, 0x33, 0x73, 0x18, 0x02, 0x00,
        0x02, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x15, 0x00,
        0x01, 0x00, 0x21, 0xd3, 0xaa, 0x09, 0x03, 0xa7,
        0x0b, 0xc2, 0x06, 0x45, 0xd9, 0x6c, 0x75, 0xc2,
        0x15, 0xa8, 0x01, 0x00, 0x03, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00, 0x16, 0x00, 0x01, 0x00, 0xe1, 0xbd,
        0x59, 0xfc, 0xbc, 0xa9, 0x95, 0xc2, 0x68, 0x79,
        0xf3, 0x75, 0xe0, 0xae, 0x6c, 0xe5, 0x04, 0x00,
        0x02, 0x00, 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c,
        0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10,
        0x48, 0x60, 0x02, 0x00, 0x00, 0x00, 0x17, 0x00,
        0x01, 0x00, 0x06, 0x52, 0xb4, 0x71, 0x70, 0x15,
        0x4e, 0xf5, 0x7f, 0x08, 0x86, 0x14, 0xe6, 0x17,
        0xd5, 0x97, 0x04, 0x00, 0x00, 0x00, 0x04, 0x5d,
        0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
        0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 0x02, 0x00,
        0x00, 0x00};

    uint8_t dcerpcbindack[] = {
        0x05, 0x00, 0x0c, 0x03,
        0x10, 0x00, 0x00, 0x00, 0x6c, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xb8, 0x10, 0xb8, 0x10,
        0xce, 0x47, 0x00, 0x00, 0x0c, 0x00, 0x5c, 0x50,
        0x49, 0x50, 0x45, 0x5c, 0x6c, 0x73, 0x61, 0x73,
        0x73, 0x00, 0xf6, 0x6e, 0x18, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x04, 0x5d, 0x88, 0x8a,
        0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00,
        0x2b, 0x10, 0x48, 0x60, 0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#if 0
    uint8_t dcerpcrequest[] = {
        0x05, 0x00, 0x00, 0x00, 0x10,
        0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00, 0x0b,
        0x00, 0x09, 0x00, 0x45, 0x00, 0x2c, 0x00, 0x4d,
        0x00, 0x73, 0x00, 0x53, 0x00, 0x59, 0x00, 0x2a,
        0x00, 0x4a, 0x00, 0x7a, 0x00, 0x3e, 0x00, 0x58,
        0x00, 0x21, 0x00, 0x4a, 0x00, 0x30, 0x00, 0x41,
        0x00, 0x4b, 0x00, 0x4b, 0x00, 0x3c, 0x00, 0x48,
        0x00, 0x24, 0x00, 0x38, 0x00, 0x54, 0x00, 0x60,
        0x00, 0x2d, 0x00, 0x29, 0x00, 0x64, 0x00, 0x5b,
        0x00, 0x77, 0x00, 0x3a, 0x00, 0x4c, 0x00, 0x24,
        0x00, 0x23, 0x00, 0x66, 0x00, 0x43, 0x00, 0x68,
        0x00, 0x22, 0x00, 0x55, 0x00, 0x29, 0x00, 0x2c,
        0x00, 0x4f, 0x00, 0x5a, 0x00, 0x50, 0x00, 0x61,
        0x00, 0x2a, 0x00, 0x6f, 0x00, 0x2f, 0x00, 0x4d,
        0x00, 0x68, 0x00, 0x3a, 0x00, 0x5c, 0x00, 0x67,
        0x00, 0x68, 0x00, 0x68, 0x00, 0x49, 0x00, 0x45,
        0x00, 0x4c, 0x00, 0x72, 0x00, 0x53, 0x00, 0x4c,
        0x00, 0x25, 0x00, 0x4d, 0x00, 0x67, 0x00, 0x2e,
        0x00, 0x4f, 0x00, 0x64, 0x00, 0x61, 0x00, 0x73,
        0x00, 0x24, 0x00, 0x46, 0x00, 0x35, 0x00, 0x2e,
        0x00, 0x45, 0x00, 0x6f, 0x00, 0x40, 0x00, 0x41,
        0x00, 0x33, 0x00, 0x38, 0x00, 0x47, 0x00, 0x71,
        0x00, 0x5a, 0x00, 0x37, 0x00, 0x7a, 0x00, 0x35,
        0x00, 0x6b, 0x00, 0x3c, 0x00, 0x26, 0x00, 0x37,
        0x00, 0x69, 0x00, 0x75, 0x00, 0x36, 0x00, 0x37,
        0x00, 0x47, 0x00, 0x21, 0x00, 0x2d, 0x00, 0x69,
        0x00, 0x37, 0x00, 0x78, 0x00, 0x5f, 0x00, 0x72,
        0x00, 0x4b, 0x00, 0x5c, 0x00, 0x74, 0x00, 0x3e,
        0x00, 0x52, 0x00, 0x7a, 0x00, 0x49, 0x00, 0x31,
        0x00, 0x5a, 0x00, 0x7b, 0x00, 0x29, 0x00, 0x3b,
        0x00, 0x78, 0x00, 0x3b, 0x00, 0x55, 0x00, 0x3e,
        0x00, 0x35, 0x00, 0x2b, 0x00, 0x4e, 0x00, 0x4f,
        0x00, 0x59, 0x00, 0x38, 0x00, 0x2a, 0x00, 0x59,
        0x00, 0x6b, 0x00, 0x42, 0x00, 0x4c, 0x00, 0x3e,
        0x00, 0x6a, 0x00, 0x49, 0x00, 0x2c, 0x00, 0x79,
        0x00, 0x6e, 0x00, 0x35, 0x00, 0x4f, 0x00, 0x49,
        0x00, 0x55, 0x00, 0x35, 0x00, 0x61, 0x00, 0x72,
        0x00, 0x77, 0x00, 0x38, 0x00, 0x32, 0x00, 0x24,
        0x00, 0x46, 0x00, 0x32, 0x00, 0x32, 0x00, 0x27,
        0x00, 0x64, 0x00, 0x5a, 0x00, 0x77, 0x00, 0x2e,
        0x00, 0x37, 0x00, 0x77, 0x00, 0x2e, 0x00, 0x28,
        0x00, 0x63, 0x00, 0x4f, 0x00, 0x67, 0x00, 0x64,
        0x00, 0x39, 0x00, 0x37, 0x00, 0x31, 0x00, 0x30,
        0x00, 0x28, 0x00, 0x2e, 0x00, 0x6f, 0x00, 0x3e,
        0x00, 0x59, 0x00, 0x28, 0x00, 0x67, 0x00, 0x52,
        0x00, 0x35, 0x00, 0x5a, 0x00, 0x7c, 0x00, 0x56,
        0x00, 0x6a, 0x00, 0x5c, 0x00, 0x3c, 0x00, 0x30,
        0x00, 0x59, 0x00, 0x5c, 0x00, 0x5e, 0x00, 0x38,
        0x00, 0x54, 0x00, 0x5c, 0x00, 0x5b, 0x00, 0x42,
        0x00, 0x62, 0x00, 0x70, 0x00, 0x34, 0x00, 0x5c,
        0x00, 0x57, 0x00, 0x7a, 0x00, 0x4b, 0x00, 0x2f,
        0x00, 0x6b, 0x00, 0x6a, 0x00, 0x4f, 0x00, 0x41,
        0x00, 0x33, 0x00, 0x52, 0x00, 0x36, 0x00, 0x27,
        0x00, 0x30, 0x00, 0x6d, 0x00, 0x4a, 0x00, 0x30,
        0x00, 0x78, 0x00, 0x46, 0x00, 0x65, 0x00, 0x4e,
        0x00, 0x29, 0x00, 0x66, 0x00, 0x3f, 0x00, 0x72,
        0x00, 0x71, 0x00, 0x75, 0x00, 0x4c, 0x00, 0x2b,
        0x00, 0x5c, 0x00, 0x46, 0x00, 0x52, 0x00, 0x7b,
        0x00, 0x5c, 0x00, 0x69, 0x00, 0x66, 0x00, 0x56,
        0x00, 0x31, 0x00, 0x2d, 0x00, 0x72, 0x00, 0x61,
        0x00, 0x68, 0x00, 0x28, 0x00, 0x7d, 0x00, 0x58,
        0x00, 0x2a, 0x00, 0x7b, 0x00, 0x28, 0x00, 0x5b,
        0x00, 0x54, 0x00, 0x3a, 0x00, 0x26, 0x00, 0x52,
        0x00, 0x44, 0x00, 0x60, 0x00, 0x50, 0x00, 0x65,
        0x00, 0x48, 0x00, 0x7d, 0x00, 0x2a, 0x00, 0x74,
        0x00, 0x49, 0x00, 0x7b, 0x00, 0x21, 0x00, 0x61,
        0x00, 0x52, 0x00, 0x43, 0x00, 0x5f, 0x00, 0x5a,
        0x00, 0x74, 0x00, 0x5c, 0x00, 0x62, 0x00, 0x68,
        0x00, 0x6c, 0x00, 0x6c, 0x00, 0x2b, 0x00, 0x6f,
        0x00, 0x7c, 0x00, 0x42, 0x00, 0x67, 0x00, 0x32,
        0x00, 0x58, 0x00, 0x35, 0x00, 0x30, 0x00, 0x2f,
        0x00, 0x2d, 0x00, 0x60, 0x00, 0x62, 0x00, 0x51,
        0x00, 0x2a, 0x00, 0x30, 0x00, 0x31, 0x00, 0x48,
        0x00, 0x5b, 0x00, 0x5b, 0x00, 0x5d, 0x00, 0x25,
        0x00, 0x58, 0x00, 0x4a, 0x00, 0x76, 0x00, 0x32,
        0x00, 0x62, 0x00, 0x27, 0x00, 0x42, 0x00, 0x40,
        0x00, 0x53, 0x00, 0x7c, 0x00, 0x7d, 0x00, 0x50,
        0x00, 0x3d, 0x00, 0x40, 0x00, 0x76, 0x00, 0x38,
        0x00, 0x58, 0x00, 0x39, 0x00, 0x63, 0x00, 0x3c,
        0x00, 0x5b, 0x00, 0x23, 0x00, 0x53, 0x00, 0x7a,
        0x00, 0x54, 0x00, 0x74, 0x00, 0x61, 0x00, 0x76,
        0x00, 0x4a, 0x00, 0x3e, 0x00, 0x33, 0x00, 0x75,
        0x00, 0x66, 0x00, 0x2d, 0x00, 0x48, 0x00, 0x33,
        0x00, 0x71, 0x00, 0x76, 0x00, 0x48, 0x00, 0x71,
        0x00, 0x41, 0x00, 0x6f, 0x00, 0x2a, 0x00, 0x67,
        0x00, 0x70, 0x00, 0x21, 0x00, 0x70, 0x00, 0x4b,
        0x00, 0x52, 0x00, 0x58, 0x00, 0x68, 0x00, 0x23,
        0x00, 0x39, 0x00, 0x46, 0x00, 0x4d, 0x00, 0x51,
        0x00, 0x57, 0x00, 0x3a, 0x00, 0x79, 0x00, 0x7b,
        0x00, 0x6c, 0x00, 0x55, 0x00, 0x33, 0x00, 0x65,
        0x00, 0x49, 0x00, 0x72, 0x00, 0x30, 0x00, 0x4f,
        0x00, 0x41, 0x00, 0x6e, 0x00, 0x31, 0x00, 0x4a,
        0x00, 0x60, 0x00, 0x79, 0x00, 0x70, 0x00, 0x4f,
        0x00, 0x58, 0x00, 0x75, 0x00, 0x44, 0x00, 0x59,
        0x00, 0x58, 0x00, 0x46, 0x00, 0x3d, 0x00, 0x46,
        0x00, 0x74, 0x00, 0x51, 0x00, 0x57, 0x00, 0x6e,
        0x00, 0x2d, 0x00, 0x47, 0x00, 0x23, 0x00, 0x45,
        0x00, 0x60, 0x00, 0x4c, 0x00, 0x72, 0x00, 0x4e,
        0x00, 0x74, 0x00, 0x40, 0x00, 0x76, 0x00, 0x75,
        0x00, 0x74, 0x00, 0x56, 0x00, 0x44, 0x00, 0x29,
        0x00, 0x62, 0x00, 0x58, 0x00, 0x31, 0x00, 0x78,
        0x00, 0x32, 0x00, 0x52, 0x00, 0x4a, 0x00, 0x6b,
        0x00, 0x55, 0x00, 0x72, 0x00, 0x6f, 0x00, 0x6f,
        0x00, 0x4a, 0x00, 0x54, 0x00, 0x7d, 0x00, 0x68,
        0x00, 0x3f, 0x00, 0x28, 0x00, 0x21, 0x00, 0x53,
        0x00, 0x48, 0x00, 0x5a, 0x00, 0x34, 0x00, 0x36,
        0x00, 0x35, 0x00, 0x64, 0x00, 0x4e, 0x00, 0x75,
        0x00, 0x69, 0x00, 0x23, 0x00, 0x75, 0x00, 0x55,
        0x00, 0x43, 0x00, 0x75, 0x00, 0x2f, 0x00, 0x73,
        0x00, 0x62, 0x00, 0x6f, 0x00, 0x37, 0x00, 0x4e,
        0x00, 0x25, 0x00, 0x25, 0x00, 0x21, 0x00, 0x3d,
        0x00, 0x3c, 0x00, 0x71, 0x00, 0x3e, 0x00, 0x3f,
        0x00, 0x30, 0x00, 0x36, 0x00, 0x62, 0x00, 0x63,
        0x00, 0x53, 0x00, 0x54, 0x00, 0x5d, 0x00, 0x61,
        0x00, 0x4c, 0x00, 0x28, 0x00, 0x2b, 0x00, 0x4c,
        0x00, 0x4e, 0x00, 0x66, 0x00, 0x5f, 0x00, 0x4b,
        0x00, 0x43, 0x00, 0x75, 0x00, 0x45, 0x00, 0x37,
        0x00, 0x28, 0x00, 0x56, 0x00, 0x36, 0x00, 0x6a,
        0x00, 0x3e, 0x00, 0x64, 0x00, 0x34, 0x00, 0x6a,
        0x00, 0x7d, 0x00, 0x4a, 0x00, 0x66, 0x00, 0x7a,
        0x00, 0x3e, 0x00, 0x75, 0x00, 0x38, 0x00, 0x7b,
        0x00, 0x42, 0x00, 0x76, 0x00, 0x29, 0x00, 0x4c,
        0x00, 0x65, 0x00, 0x2e, 0x00, 0x32, 0x00, 0x4b,
        0x00, 0x2b, 0x00, 0x51, 0x00, 0x47, 0x00, 0x22,
        0x00, 0x48, 0x00, 0x3d, 0x00, 0x49, 0x00, 0x44,
        0x00, 0x5d, 0x00, 0x59, 0x00, 0x63, 0x00, 0x5c,
        0x00, 0x24, 0x00, 0x35, 0x00, 0x34, 0x00, 0x70,
        0x00, 0x69, 0x00};
    uint32_t requestlen = sizeof(dcerpcrequest);
#endif

    uint32_t bindlen = sizeof(dcerpcbind);
    uint32_t bindacklen = sizeof(dcerpcbindack);
    TcpSession ssn;
    struct uuid_entry *uuid_entry;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    StreamL7DataPtrInit(&ssn,StreamL7GetStorageSize());
    f.protoctx = (void *)&ssn;

    int r = AppLayerParse(&f, ALPROTO_DCERPC, STREAM_TOSERVER|STREAM_START, dcerpcbind, bindlen, FALSE);
    if (r != 0) {
        printf("dcerpc header check returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    DCERPCState *dcerpc_state = ssn.aldata[AlpGetStateIdx(ALPROTO_DCERPC)];
    if (dcerpc_state == NULL) {
        printf("no dcerpc state: ");
        result = 0;
        goto end;
    }

    if (dcerpc_state->dcerpc.rpc_vers != 5) {
        printf("expected dcerpc version 0x05, got 0x%02x : ",
                dcerpc_state->dcerpc.rpc_vers);
        result = 0;
        goto end;
    }

    if (dcerpc_state->dcerpc.type != BIND) {
        printf("expected dcerpc type 0x%02x , got 0x%02x : ", BIND, dcerpc_state->dcerpc.type);
        result = 0;
        goto end;
    }

    if (dcerpc_state->dcerpc.frag_length != 1084) {
        printf("expected dcerpc frag_length 0x%02x , got 0x%02x : ", 1084, dcerpc_state->dcerpc.frag_length);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_DCERPC, STREAM_TOCLIENT, dcerpcbindack, bindacklen, FALSE);
    if (r != 0) {
        printf("dcerpc header check returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }
    if (dcerpc_state->dcerpc.type != BIND_ACK) {
        printf("expected dcerpc type 0x%02x , got 0x%02x : ", BIND_ACK, dcerpc_state->dcerpc.type);
        result = 0;
        goto end;
    }

    if (dcerpc_state->dcerpc.frag_length != 620) {
        printf("expected dcerpc frag_length 0x%02x , got 0x%02x : ", 620, dcerpc_state->dcerpc.frag_length);
        result = 0;
        goto end;
    }
    TAILQ_FOREACH(uuid_entry, &dcerpc_state->uuid_list, next) {
        printUUID("BIND_ACK", uuid_entry);
    }
end:
    return result;
}

void DCERPCParserRegisterTests(void) {
    printf("DCERPCParserRegisterTests\n");
    UtRegisterTest("DCERPCParserTest01", DCERPCParserTest01, 1);
}
#endif

