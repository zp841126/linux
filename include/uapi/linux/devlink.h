/*
 * include/uapi/linux/devlink.h - Network physical device Netlink interface
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Jiri Pirko <jiri@mellanox.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI_LINUX_DEVLINK_H_
#define _UAPI_LINUX_DEVLINK_H_

#define DEVLINK_GENL_NAME "devlink"
#define DEVLINK_GENL_VERSION 0x1
#define DEVLINK_GENL_MCGRP_CONFIG_NAME "config"

enum devlink_command {
	/* don't change the order or add anything between, this is ABI! */
	DEVLINK_CMD_UNSPEC,

	DEVLINK_CMD_GET,		/* can dump */
	DEVLINK_CMD_SET,
	DEVLINK_CMD_NEW,
	DEVLINK_CMD_DEL,

	DEVLINK_CMD_PORT_GET,		/* can dump */
	DEVLINK_CMD_PORT_SET,
	DEVLINK_CMD_PORT_NEW,
	DEVLINK_CMD_PORT_DEL,

	DEVLINK_CMD_PORT_SPLIT,
	DEVLINK_CMD_PORT_UNSPLIT,

	DEVLINK_CMD_SB_GET,		/* can dump */
	DEVLINK_CMD_SB_SET,
	DEVLINK_CMD_SB_NEW,
	DEVLINK_CMD_SB_DEL,

	DEVLINK_CMD_SB_POOL_GET,	/* can dump */
	DEVLINK_CMD_SB_POOL_SET,
	DEVLINK_CMD_SB_POOL_NEW,
	DEVLINK_CMD_SB_POOL_DEL,

	DEVLINK_CMD_SB_PORT_POOL_GET,	/* can dump */
	DEVLINK_CMD_SB_PORT_POOL_SET,
	DEVLINK_CMD_SB_PORT_POOL_NEW,
	DEVLINK_CMD_SB_PORT_POOL_DEL,

	DEVLINK_CMD_SB_TC_POOL_BIND_GET,	/* can dump */
	DEVLINK_CMD_SB_TC_POOL_BIND_SET,
	DEVLINK_CMD_SB_TC_POOL_BIND_NEW,
	DEVLINK_CMD_SB_TC_POOL_BIND_DEL,

	/* Shared buffer occupancy monitoring commands */
	DEVLINK_CMD_SB_OCC_SNAPSHOT,
	DEVLINK_CMD_SB_OCC_MAX_CLEAR,

	DEVLINK_CMD_ESWITCH_GET,
#define DEVLINK_CMD_ESWITCH_MODE_GET /* obsolete, never use this! */ \
	DEVLINK_CMD_ESWITCH_GET

	DEVLINK_CMD_ESWITCH_SET,
#define DEVLINK_CMD_ESWITCH_MODE_SET /* obsolete, never use this! */ \
	DEVLINK_CMD_ESWITCH_SET

	DEVLINK_CMD_DPIPE_TABLE_GET,
	DEVLINK_CMD_DPIPE_ENTRIES_GET,
	DEVLINK_CMD_DPIPE_HEADERS_GET,
	DEVLINK_CMD_DPIPE_TABLE_COUNTERS_SET,
	DEVLINK_CMD_RESOURCE_SET,
	DEVLINK_CMD_RESOURCE_DUMP,

	/* Hot driver reload, makes configuration changes take place. The
	 * devlink instance is not released during the process.
	 */
	DEVLINK_CMD_RELOAD,

	/* add new commands above here */
	__DEVLINK_CMD_MAX,
	DEVLINK_CMD_MAX = __DEVLINK_CMD_MAX - 1
};

enum devlink_port_type {
	DEVLINK_PORT_TYPE_NOTSET,
	DEVLINK_PORT_TYPE_AUTO,
	DEVLINK_PORT_TYPE_ETH,
	DEVLINK_PORT_TYPE_IB,
};

enum devlink_sb_pool_type {
	DEVLINK_SB_POOL_TYPE_INGRESS,
	DEVLINK_SB_POOL_TYPE_EGRESS,
};

/* static threshold - limiting the maximum number of bytes.
 * dynamic threshold - limiting the maximum number of bytes
 *   based on the currently available free space in the shared buffer pool.
 *   In this mode, the maximum quota is calculated based
 *   on the following formula:
 *     max_quota = alpha / (1 + alpha) * Free_Buffer
 *   While Free_Buffer is the amount of none-occupied buffer associated to
 *   the relevant pool.
 *   The value range which can be passed is 0-20 and serves
 *   for computation of alpha by following formula:
 *     alpha = 2 ^ (passed_value - 10)
 */

enum devlink_sb_threshold_type {
	DEVLINK_SB_THRESHOLD_TYPE_STATIC,
	DEVLINK_SB_THRESHOLD_TYPE_DYNAMIC,
};

#define DEVLINK_SB_THRESHOLD_TO_ALPHA_MAX 20

enum devlink_eswitch_mode {
	DEVLINK_ESWITCH_MODE_LEGACY,
	DEVLINK_ESWITCH_MODE_SWITCHDEV,
};

enum devlink_eswitch_inline_mode {
	DEVLINK_ESWITCH_INLINE_MODE_NONE,
	DEVLINK_ESWITCH_INLINE_MODE_LINK,
	DEVLINK_ESWITCH_INLINE_MODE_NETWORK,
	DEVLINK_ESWITCH_INLINE_MODE_TRANSPORT,
};

enum devlink_eswitch_encap_mode {
	DEVLINK_ESWITCH_ENCAP_MODE_NONE,
	DEVLINK_ESWITCH_ENCAP_MODE_BASIC,
};

enum devlink_attr {
	/* don't change the order or add anything between, this is ABI! */
	DEVLINK_ATTR_UNSPEC,

	/* bus name + dev name together are a handle for devlink entity */
	DEVLINK_ATTR_BUS_NAME,			/* string */
	DEVLINK_ATTR_DEV_NAME,			/* string */

	DEVLINK_ATTR_PORT_INDEX,		/* u32 */
	DEVLINK_ATTR_PORT_TYPE,			/* u16 */
	DEVLINK_ATTR_PORT_DESIRED_TYPE,		/* u16 */
	DEVLINK_ATTR_PORT_NETDEV_IFINDEX,	/* u32 */
	DEVLINK_ATTR_PORT_NETDEV_NAME,		/* string */
	DEVLINK_ATTR_PORT_IBDEV_NAME,		/* string */
	DEVLINK_ATTR_PORT_SPLIT_COUNT,		/* u32 */
	DEVLINK_ATTR_PORT_SPLIT_GROUP,		/* u32 */
	DEVLINK_ATTR_SB_INDEX,			/* u32 */
	DEVLINK_ATTR_SB_SIZE,			/* u32 */
	DEVLINK_ATTR_SB_INGRESS_POOL_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_EGRESS_POOL_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_INGRESS_TC_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_EGRESS_TC_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_POOL_INDEX,		/* u16 */
	DEVLINK_ATTR_SB_POOL_TYPE,		/* u8 */
	DEVLINK_ATTR_SB_POOL_SIZE,		/* u32 */
	DEVLINK_ATTR_SB_POOL_THRESHOLD_TYPE,	/* u8 */
	DEVLINK_ATTR_SB_THRESHOLD,		/* u32 */
	DEVLINK_ATTR_SB_TC_INDEX,		/* u16 */
	DEVLINK_ATTR_SB_OCC_CUR,		/* u32 */
	DEVLINK_ATTR_SB_OCC_MAX,		/* u32 */
	DEVLINK_ATTR_ESWITCH_MODE,		/* u16 */
	DEVLINK_ATTR_ESWITCH_INLINE_MODE,	/* u8 */

	DEVLINK_ATTR_DPIPE_TABLES,		/* nested */
	DEVLINK_ATTR_DPIPE_TABLE,		/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_TABLE_SIZE,		/* u64 */
	DEVLINK_ATTR_DPIPE_TABLE_MATCHES,	/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_ACTIONS,	/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_COUNTERS_ENABLED,	/* u8 */

	DEVLINK_ATTR_DPIPE_ENTRIES,		/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY,		/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_INDEX,		/* u64 */
	DEVLINK_ATTR_DPIPE_ENTRY_MATCH_VALUES,	/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_ACTION_VALUES,	/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_COUNTER,	/* u64 */

	DEVLINK_ATTR_DPIPE_MATCH,		/* nested */
	DEVLINK_ATTR_DPIPE_MATCH_VALUE,		/* nested */
	DEVLINK_ATTR_DPIPE_MATCH_TYPE,		/* u32 */

	DEVLINK_ATTR_DPIPE_ACTION,		/* nested */
	DEVLINK_ATTR_DPIPE_ACTION_VALUE,	/* nested */
	DEVLINK_ATTR_DPIPE_ACTION_TYPE,		/* u32 */

	DEVLINK_ATTR_DPIPE_VALUE,
	DEVLINK_ATTR_DPIPE_VALUE_MASK,
	DEVLINK_ATTR_DPIPE_VALUE_MAPPING,	/* u32 */

	DEVLINK_ATTR_DPIPE_HEADERS,		/* nested */
	DEVLINK_ATTR_DPIPE_HEADER,		/* nested */
	DEVLINK_ATTR_DPIPE_HEADER_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_HEADER_ID,		/* u32 */
	DEVLINK_ATTR_DPIPE_HEADER_FIELDS,	/* nested */
	DEVLINK_ATTR_DPIPE_HEADER_GLOBAL,	/* u8 */
	DEVLINK_ATTR_DPIPE_HEADER_INDEX,	/* u32 */

	DEVLINK_ATTR_DPIPE_FIELD,		/* nested */
	DEVLINK_ATTR_DPIPE_FIELD_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_FIELD_ID,		/* u32 */
	DEVLINK_ATTR_DPIPE_FIELD_BITWIDTH,	/* u32 */
	DEVLINK_ATTR_DPIPE_FIELD_MAPPING_TYPE,	/* u32 */

	DEVLINK_ATTR_PAD,

	DEVLINK_ATTR_ESWITCH_ENCAP_MODE,	/* u8 */
	DEVLINK_ATTR_RESOURCE_LIST,		/* nested */
	DEVLINK_ATTR_RESOURCE,			/* nested */
	DEVLINK_ATTR_RESOURCE_NAME,		/* string */
	DEVLINK_ATTR_RESOURCE_ID,		/* u64 */
	DEVLINK_ATTR_RESOURCE_SIZE,		/* u64 */
	DEVLINK_ATTR_RESOURCE_SIZE_NEW,		/* u64 */
	DEVLINK_ATTR_RESOURCE_SIZE_VALID,	/* u8 */
	DEVLINK_ATTR_RESOURCE_SIZE_MIN,		/* u64 */
	DEVLINK_ATTR_RESOURCE_SIZE_MAX,		/* u64 */
	DEVLINK_ATTR_RESOURCE_SIZE_GRAN,        /* u64 */
	DEVLINK_ATTR_RESOURCE_UNIT,		/* u8 */
	DEVLINK_ATTR_RESOURCE_OCC,		/* u64 */
	DEVLINK_ATTR_DPIPE_TABLE_RESOURCE_ID,	/* u64 */
	DEVLINK_ATTR_DPIPE_TABLE_RESOURCE_UNITS,/* u64 */

	/* add new attributes above here, update the policy in devlink.c */

	__DEVLINK_ATTR_MAX,
	DEVLINK_ATTR_MAX = __DEVLINK_ATTR_MAX - 1
};

/* Mapping between internal resource described by the field and system
 * structure
 */
enum devlink_dpipe_field_mapping_type {
	DEVLINK_DPIPE_FIELD_MAPPING_TYPE_NONE,
	DEVLINK_DPIPE_FIELD_MAPPING_TYPE_IFINDEX,
};

/* Match type - specify the type of the match */
enum devlink_dpipe_match_type {
	DEVLINK_DPIPE_MATCH_TYPE_FIELD_EXACT,
};

/* Action type - specify the action type */
enum devlink_dpipe_action_type {
	DEVLINK_DPIPE_ACTION_TYPE_FIELD_MODIFY,
};

enum devlink_dpipe_field_ethernet_id {
	DEVLINK_DPIPE_FIELD_ETHERNET_DST_MAC,
};

enum devlink_dpipe_field_ipv4_id {
	DEVLINK_DPIPE_FIELD_IPV4_DST_IP,
};

enum devlink_dpipe_field_ipv6_id {
	DEVLINK_DPIPE_FIELD_IPV6_DST_IP,
};

enum devlink_dpipe_header_id {
	DEVLINK_DPIPE_HEADER_ETHERNET,
	DEVLINK_DPIPE_HEADER_IPV4,
	DEVLINK_DPIPE_HEADER_IPV6,
};

enum devlink_resource_unit {
	DEVLINK_RESOURCE_UNIT_ENTRY,
};

#endif /* _UAPI_LINUX_DEVLINK_H_ */
