/*
 * OpenWrt UCI-backed CMM QM port shaper configuration.
 *
 * This is deliberately compiled only for product builds that opt in with
 * --enable-openwrt-uci-qos.  Keeping it here, rather than in an init script,
 * makes CMM responsible for restoring the state it programs after procd
 * respawns it.
 */

#include "cmm.h"

#ifdef CMM_OPENWRT_UCI_QOS
#include <uci.h>
#include <errno.h>

#define CMMQOS_MAX_SHAPERS GEM_PORTS
#define CMMQOS_RATE_MAX 10000000U
#define CMMQOS_BUCKET_MAX 65535U
/*
 * The first product scope owns one CEETM channel for the sole configured
 * port shaper.  CMM's CLI calls this "channel 1"; the FPP/CDX ABI is
 * zero-based, so it is channel index 0.  Do not infer another free channel:
 * CDX rejects assignments already owned by another port, which is the safe
 * failure mode until multi-port ownership semantics are designed.
 */
#define CMMQOS_CHANNEL_CLI 1U
#define CMMQOS_CHANNEL_INDEX (CMMQOS_CHANNEL_CLI - 1U)

struct cmmqos_shaper {
	char device[IFNAMSIZ];
	uint32_t rate;
	uint32_t bucketsize;
	int enabled;
};

static const char *cmmqos_option(struct uci_section *section, const char *name)
{
	struct uci_element *element;

	uci_foreach_element(&section->options, element) {
		struct uci_option *option = uci_to_option(element);

		if (option->type == UCI_TYPE_STRING && !strcmp(option->e.name, name))
			return option->v.string;
	}
	return NULL;
}

static int cmmqos_u32(const char *value, uint32_t max, uint32_t *result)
{
	char *end;
	unsigned long parsed;

	if (!value || !*value || value[0] == '-')
		return -1;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || *end || parsed == 0 || parsed > max)
		return -1;
	*result = parsed;
	return 0;
}

static int cmmqos_enabled(const char *value, int *enabled)
{
	if (!value || !strcmp(value, "0")) {
		*enabled = 0;
		return 0;
	}
	if (!strcmp(value, "1")) {
		*enabled = 1;
		return 0;
	}
	return -1;
}

static int cmmqos_is_physical_port(const char *device)
{
	int i;

	for (i = 0; i < GEM_PORTS; i++) {
		if (port_table[i].enable && !strcmp(port_table[i].ifname, device))
			return 1;
	}
	return 0;
}

static int cmmqos_validate(struct uci_package *package,
		struct cmmqos_shaper *shapers, size_t *count)
{
	struct uci_element *element;

	*count = 0;
	uci_foreach_element(&package->sections, element) {
		struct uci_section *section = uci_to_section(element);
		struct cmmqos_shaper *shaper;
		const char *device;
		const char *enabled;
		size_t i;

		if (strcmp(section->type, "shaper"))
			continue;
		if (*count == CMMQOS_MAX_SHAPERS) {
			cmm_print(DEBUG_ERROR, "cmmqos: too many shaper sections\n");
			return CMMD_ERR_WRONG_COMMAND_PARAM;
		}
		shaper = &shapers[(*count)++];
		memset(shaper, 0, sizeof(*shaper));
		device = cmmqos_option(section, "device");
		enabled = cmmqos_option(section, "enabled");
		if (!device || strlen(device) >= sizeof(shaper->device) ||
			!cmmqos_is_physical_port(device) || cmmqos_enabled(enabled, &shaper->enabled)) {
			cmm_print(DEBUG_ERROR, "cmmqos: invalid device or enabled value in section %s\n", section->e.name);
			return CMMD_ERR_WRONG_COMMAND_PARAM;
		}
		for (i = 0; i + 1 < *count; i++) {
			if (!strcmp(shapers[i].device, device)) {
				cmm_print(DEBUG_ERROR, "cmmqos: duplicate physical port %s\n", device);
				return CMMD_ERR_DUPLICATE;
			}
		}
		STR_TRUNC_COPY(shaper->device, device, sizeof(shaper->device));
		if (shaper->enabled &&
			(cmmqos_u32(cmmqos_option(section, "rate"), CMMQOS_RATE_MAX, &shaper->rate) ||
			 cmmqos_u32(cmmqos_option(section, "bucketsize"), CMMQOS_BUCKET_MAX, &shaper->bucketsize))) {
			cmm_print(DEBUG_ERROR, "cmmqos: invalid rate or bucketsize in section %s\n", section->e.name);
			return CMMD_ERR_WRONG_COMMAND_PARAM;
		}
	}
	return CMMD_ERR_OK;
}

static int cmmqos_program(FCI_CLIENT *fci_handle,
		const struct cmmqos_shaper *shaper, const char **operation)
{
	fpp_qm_qos_enable_cmd_t qos;
	fpp_qm_shaper_cfg_cmd_t config;
	fpp_qm_chnl_assign_cmd_t assign;
	int rc;

	if (!shaper->enabled) {
		fpp_qm_reset_cmd_t reset;

		memset(&reset, 0, sizeof(reset));
		STR_TRUNC_COPY(reset.interface, shaper->device, sizeof(reset.interface));
		*operation = "reset";
		rc = fci_write(fci_handle, FPP_CMD_QM_RESET, sizeof(reset), (unsigned short *)&reset);
		return rc;
	}

	/* CEETM refuses QoS enable until a channel is assigned to this port. */
	memset(&assign, 0, sizeof(assign));
	STR_TRUNC_COPY(assign.interface, shaper->device, sizeof(assign.interface));
	assign.channel_num = CMMQOS_CHANNEL_INDEX;
	*operation = "assign channel 1";
	rc = fci_write(fci_handle, FPP_CMD_QM_CHNL_ASSIGN, sizeof(assign),
		(unsigned short *)&assign);
	if (rc != FPP_ERR_OK)
		return rc;

	memset(&qos, 0, sizeof(qos));
	STR_TRUNC_COPY(qos.interface, shaper->device, sizeof(qos.interface));
	qos.enable = 1;
	*operation = "enable qos";
	rc = fci_write(fci_handle, FPP_CMD_QM_QOSENABLE, sizeof(qos), (unsigned short *)&qos);
	if (rc != FPP_ERR_OK)
		return rc;

	memset(&config, 0, sizeof(config));
	STR_TRUNC_COPY(config.interface, shaper->device, sizeof(config.interface));
	config.enable = SHAPER_ON;
	config.rate = shaper->rate;
	config.bsize = shaper->bucketsize;
	config.cfg_flags = PORT_SHAPER_CFG | SHAPER_CFG_VALID | RATE_VALID | BSIZE_VALID;
	*operation = "configure port shaper";
	return fci_write(fci_handle, FPP_CMD_QM_SHAPER_CFG, sizeof(config), (unsigned short *)&config);
}

int cmmQmUciLoad(FCI_CLIENT *fci_handle)
{
	struct uci_context *ctx;
	struct uci_package *package = NULL;
	struct cmmqos_shaper shapers[CMMQOS_MAX_SHAPERS];
	size_t count;
	size_t i;
	const char *operation;
	int rc;

	ctx = uci_alloc_context();
	if (!ctx)
		return CMMD_ERR_MEMORY;
	if (uci_load(ctx, "cmmqos", &package) != UCI_OK) {
		/* A missing config is valid for upgrades before the default is installed. */
		rc = (ctx->err == UCI_ERR_NOTFOUND) ? CMMD_ERR_OK : CMMD_ERR_WRONG_COMMAND_PARAM;
		if (rc != CMMD_ERR_OK)
			cmm_print(DEBUG_ERROR, "cmmqos: unable to load /etc/config/cmmqos\n");
		goto out;
	}
	rc = cmmqos_validate(package, shapers, &count);
	if (rc != CMMD_ERR_OK)
		goto out;
	for (i = 0; i < count; i++) {
		operation = "unknown operation";
		rc = cmmqos_program(fci_handle, &shapers[i], &operation);
		if (rc != FPP_ERR_OK) {
			cmm_print(DEBUG_ERROR, "cmmqos: %s failed on %s (rc %d)\n",
				operation, shapers[i].device, rc);
			goto out;
		}
	}
	rc = CMMD_ERR_OK;
out:
	if (package)
		uci_unload(ctx, package);
	uci_free_context(ctx);
	return rc;
}

int cmmQmUciReload(void)
{
	daemon_handle_t handle;
	unsigned short result = CMMD_ERR_UNKNOWN;
	int bytes;

	handle = cmm_open();
	if (!handle)
		return CMMD_ERR_NOT_CONFIGURED;
	bytes = cmmSendToDaemon(handle, CMMD_CMD_QM_UCI_RELOAD, NULL, 0, &result);
	cmm_close(handle);
	return bytes == sizeof(result) ? result : CMMD_ERR_UNKNOWN;
}

#else
int cmmQmUciLoad(FCI_CLIENT *fci_handle)
{
	(void)fci_handle;
	return CMMD_ERR_OK;
}

int cmmQmUciReload(void)
{
	return CMMD_ERR_OK;
}
#endif
