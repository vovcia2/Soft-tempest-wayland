#define _GNU_SOURCE
#include "config.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SOFT_TEMPEST_VERSION
#define SOFT_TEMPEST_VERSION "dev"
#endif

void config_defaults(struct config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->amplitude = 0.25f;
    cfg->mode = MODE_WHITE;
    cfg->fps_cap = 0;
}

int config_parse_mode(const char *name)
{
    if (!strcmp(name, "white") || !strcmp(name, "0"))
        return MODE_WHITE;
    if (!strcmp(name, "hf") || !strcmp(name, "1"))
        return MODE_HF;
    if (!strcmp(name, "temporal") || !strcmp(name, "2"))
        return MODE_TEMPORAL;
    return -1;
}

const char *config_mode_name(int mode)
{
    switch (mode) {
    case MODE_WHITE: return "white";
    case MODE_HF: return "hf";
    case MODE_TEMPORAL: return "temporal";
    default: return "?";
    }
}

static int parse_amplitude(const char *s, float *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end != '\0' || isnan(v))
        return -1;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    *out = (float)v;
    return 0;
}

static int parse_fps(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < 0 || v > 1000)
        return -1;
    *out = (int)v;
    return 0;
}

static void set_string(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && *src) ? strdup(src) : NULL;
}

/* Apply a key/value pair; shared by the config file and the CLI.
 * Returns 0 on success, -1 on an invalid value, -2 on an unknown key. */
static int apply_kv(struct config *cfg, const char *key, const char *value)
{
    if (!strcmp(key, "amplitude"))
        return parse_amplitude(value, &cfg->amplitude);
    if (!strcmp(key, "mode")) {
        int m = config_parse_mode(value);
        if (m < 0) return -1;
        cfg->mode = m;
        return 0;
    }
    if (!strcmp(key, "fps-cap") || !strcmp(key, "fps_cap") || !strcmp(key, "fps"))
        return parse_fps(value, &cfg->fps_cap);
    if (!strcmp(key, "output")) {
        set_string(&cfg->output, value);
        return 0;
    }
    if (!strcmp(key, "verbose")) {
        if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes"))
            cfg->verbose = true;
        else if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "no"))
            cfg->verbose = false;
        else
            return -1;
        return 0;
    }
    return -2;
}

static char *default_config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *path = NULL;
    if (xdg && *xdg) {
        if (asprintf(&path, "%s/soft-tempest/config", xdg) < 0)
            return NULL;
        return path;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;
    if (asprintf(&path, "%s/.config/soft-tempest/config", home) < 0)
        return NULL;
    return path;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int config_load_file(struct config *cfg)
{
    if (cfg->no_config)
        return 0;

    char *path = cfg->config_path ? strdup(cfg->config_path)
                                  : default_config_path();
    if (!path)
        return 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        int ret = 0;
        if (cfg->config_path) {
            /* An explicitly requested file must exist. */
            fprintf(stderr, "soft-tempest: cannot open config file %s: %s\n",
                    path, strerror(errno));
            ret = -1;
        }
        free(path);
        return ret;
    }

    int ret = 0;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key = value\n", path, lineno);
            ret = -1;
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        /* Strip optional surrounding quotes from the value. */
        size_t vl = strlen(value);
        if (vl >= 2 && ((value[0] == '"' && value[vl - 1] == '"') ||
                        (value[0] == '\'' && value[vl - 1] == '\''))) {
            value[vl - 1] = '\0';
            value++;
        }
        int r = apply_kv(cfg, key, value);
        if (r == -2) {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
            ret = -1;
        } else if (r == -1) {
            fprintf(stderr, "%s:%d: invalid value '%s' for '%s'\n",
                    path, lineno, value, key);
            ret = -1;
        }
    }
    fclose(f);
    if (cfg->verbose && ret == 0)
        fprintf(stderr, "soft-tempest: loaded config from %s\n", path);
    free(path);
    return ret;
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
        "Usage: %s [options]\n"
        "\n"
        "Full-screen, click-through per-frame noise overlay for Wayland compositors\n"
        "implementing wlr-layer-shell (soft-TEMPEST display masking).\n"
        "\n"
        "Options:\n"
        "  -a, --amplitude <0..1>     noise opacity (default 0.25)\n"
        "  -m, --mode <name>          white | hf | temporal (default white)\n"
        "  -f, --fps-cap <N>          limit rendering to N frames/s (0 = compositor rate)\n"
        "  -o, --output <name>        only cover the wl_output with this name (e.g. DP-1)\n"
        "  -c, --config <path>        read settings from this file instead of\n"
        "                             $XDG_CONFIG_HOME/soft-tempest/config\n"
        "      --no-config            ignore any config file\n"
        "  -v, --verbose              log what is happening to stderr\n"
        "  -h, --help                 show this help and exit\n"
        "  -V, --version              print version and exit\n"
        "\n"
        "Command-line options override values from the config file. The config file\n"
        "uses 'key = value' lines with the keys: amplitude, mode, fps-cap, output, verbose.\n"
        "\n"
        "Send SIGINT (Ctrl+C) or SIGTERM to remove the overlay and exit.\n",
        argv0);
}

int config_parse_args(struct config *cfg, int argc, char **argv)
{
    static const struct option longopts[] = {
        { "amplitude", required_argument, NULL, 'a' },
        { "mode",      required_argument, NULL, 'm' },
        { "fps-cap",   required_argument, NULL, 'f' },
        { "output",    required_argument, NULL, 'o' },
        { "config",    required_argument, NULL, 'c' },
        { "no-config", no_argument,       NULL, 'N' },
        { "verbose",   no_argument,       NULL, 'v' },
        { "help",      no_argument,       NULL, 'h' },
        { "version",   no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "a:m:f:o:c:vhV", longopts, NULL)) != -1) {
        switch (c) {
        case 'a':
            if (parse_amplitude(optarg, &cfg->amplitude) < 0) {
                fprintf(stderr, "soft-tempest: invalid amplitude '%s'\n", optarg);
                return -1;
            }
            break;
        case 'm': {
            int m = config_parse_mode(optarg);
            if (m < 0) {
                fprintf(stderr, "soft-tempest: unknown mode '%s' "
                        "(expected white, hf or temporal)\n", optarg);
                return -1;
            }
            cfg->mode = m;
            break;
        }
        case 'f':
            if (parse_fps(optarg, &cfg->fps_cap) < 0) {
                fprintf(stderr, "soft-tempest: invalid fps cap '%s'\n", optarg);
                return -1;
            }
            break;
        case 'o':
            set_string(&cfg->output, optarg);
            break;
        case 'c':
            set_string(&cfg->config_path, optarg);
            break;
        case 'N':
            cfg->no_config = true;
            break;
        case 'v':
            cfg->verbose = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 1;
        case 'V':
            printf("soft-tempest %s\n", SOFT_TEMPEST_VERSION);
            return 1;
        default:
            usage(stderr, argv[0]);
            return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "soft-tempest: unexpected argument '%s'\n", argv[optind]);
        usage(stderr, argv[0]);
        return -1;
    }
    return 0;
}

int config_load(struct config *cfg, int argc, char **argv)
{
    /* First pass: only to learn --config/--no-config/--verbose/--help. */
    struct config probe;
    config_defaults(&probe);
    int r = config_parse_args(&probe, argc, argv);
    if (r != 0) {
        config_free(&probe);
        return r;
    }
    cfg->config_path = probe.config_path;
    probe.config_path = NULL;
    cfg->no_config = probe.no_config;
    cfg->verbose = probe.verbose;
    config_free(&probe);

    if (config_load_file(cfg) < 0)
        return -1;

    /* Second pass: CLI overrides the file. */
    return config_parse_args(cfg, argc, argv);
}

void config_free(struct config *cfg)
{
    free(cfg->output);
    free(cfg->config_path);
    cfg->output = NULL;
    cfg->config_path = NULL;
}
