#ifndef SOFT_TEMPEST_CONFIG_H
#define SOFT_TEMPEST_CONFIG_H

#include <stdbool.h>

enum noise_mode {
    MODE_WHITE = 0,
    MODE_HF = 1,
    MODE_TEMPORAL = 2,
};

struct config {
    float amplitude;   /* 0.0 .. 1.0 */
    int mode;          /* enum noise_mode */
    int fps_cap;       /* 0 = follow compositor refresh */
    char *output;      /* restrict to this wl_output name, NULL = all */
    char *config_path; /* explicit --config path, NULL = XDG default */
    bool no_config;    /* --no-config: skip the config file entirely */
    bool verbose;
};

/* Fill in built-in defaults. */
void config_defaults(struct config *cfg);

/* Parse the command line. Returns 0 to continue, 1 when the program should
 * exit successfully (e.g. --help), -1 on a usage error (message printed). */
int config_parse_args(struct config *cfg, int argc, char **argv);

/* Load $XDG_CONFIG_HOME/soft-tempest/config (or cfg->config_path). Returns 0
 * on success or if no file exists, -1 on a parse error (message printed). */
int config_load_file(struct config *cfg);

/* Load the config file first, then apply command-line overrides on top. */
int config_load(struct config *cfg, int argc, char **argv);

int config_parse_mode(const char *name);
const char *config_mode_name(int mode);
void config_free(struct config *cfg);

#endif
