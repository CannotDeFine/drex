/* DPA flow demo: kernel launch (main). */

#include <stdlib.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#include "dpa_common.h"

DOCA_LOG_REGISTER(KERNEL_LAUNCH::MAIN);

doca_error_t kernel_launch(struct dpa_resources *resources);

int main(int argc, char **argv)
{
	struct dpa_config cfg = {{0}};
	struct dpa_resources resources = {0};
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	static const int k_verbose = 0;

	/* Set default value for device name */
	strcpy(cfg.device_name, DEVICE_DEFAULT_NAME);

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	if (k_verbose)
		DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init("doca_dpa_kernel_launch", &cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	/* Register DPA params */
	result = register_dpa_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Allocating resources */
	result = allocate_dpa_resources(&cfg, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to Allocate DPA Resources: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* Running sample */
	result = kernel_launch(&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("kernel_launch() encountered an error: %s", doca_error_get_descr(result));
		goto dpa_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpa_cleanup:
	/* Destroying DPA resources */
	result = destroy_dpa_resources(&resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA DPA resources: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
	}
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (k_verbose) {
		if (exit_status == EXIT_SUCCESS)
			DOCA_LOG_INFO("Sample finished successfully");
		else
			DOCA_LOG_INFO("Sample finished with errors");
	}
	return exit_status;
}
