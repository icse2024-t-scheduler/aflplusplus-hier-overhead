/*
   american fuzzy lop++ - fuzzer code
   --------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include "cmplog.h"
#include <limits.h>
#ifndef USEMMAP
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <sys/ipc.h>
  #include <sys/shm.h>
#endif

#ifdef PROFILING
extern u64 time_spent_working;
#endif

#ifdef CALCULATE_OVERHEAD
  timestamp_t get_timestamp () {
    struct timeval now;
    gettimeofday (&now, NULL);
    return  now.tv_usec + (timestamp_t)now.tv_sec * 1000000;
  }
#endif

static void at_exit() {

  int   i;
  char *list[4] = {SHM_ENV_VAR, SHM_FUZZ_ENV_VAR, CMPLOG_SHM_ENV_VAR, NULL};
  char *ptr;

  ptr = getenv(CPU_AFFINITY_ENV_VAR);
  if (ptr && *ptr) unlink(ptr);

  ptr = getenv("__AFL_TARGET_PID1");
  if (ptr && *ptr && (i = atoi(ptr)) > 0) kill(i, SIGKILL);

  ptr = getenv("__AFL_TARGET_PID2");
  if (ptr && *ptr && (i = atoi(ptr)) > 0) kill(i, SIGKILL);

  i = 0;
  while (list[i] != NULL) {

    ptr = getenv(list[i]);
    if (ptr && *ptr) {

#ifdef USEMMAP

      shm_unlink(ptr);

#else

      shmctl(atoi(ptr), IPC_RMID, NULL);

#endif

    }

    i++;

  }

}

/* Display usage hints. */

static void usage(u8 *argv0, int more_help) {

  SAYF(
      "\n%s [ options ] -- /path/to/fuzzed_app [ ... ]\n\n"

      "Required parameters:\n"
      "  -i dir        - input directory with test cases\n"
      "  -o dir        - output directory for fuzzer findings\n\n"

      "Execution control settings:\n"
      "  -p schedule   - power schedules compute a seed's performance score. "
      "<explore\n"
      "                  (default), fast, coe, lin, quad, exploit, mmopt, "
      "rare, seek>\n"
      "                  see docs/power_schedules.md\n"
      "  -f file       - location read by the fuzzed program (default: stdin "
      "or @@)\n"
      "  -t msec       - timeout for each run (auto-scaled, 50-%d ms)\n"
      "  -m megs       - memory limit for child process (%d MB)\n"
      "  -Q            - use binary-only instrumentation (QEMU mode)\n"
      "  -U            - use unicorn-based instrumentation (Unicorn mode)\n"
      "  -W            - use qemu-based instrumentation with Wine (Wine "
      "mode)\n\n"

      "Mutator settings:\n"
      "  -L minutes    - use MOpt(imize) mode and set the time limit for "
      "entering the\n"
      "                  pacemaker mode (minutes of no new paths). 0 = "
      "immediately,\n"
      "                  -1 = immediately and together with normal mutation).\n"
      "                  See docs/README.MOpt.md\n"
      "  -c program    - enable CmpLog by specifying a binary compiled for "
      "it.\n"
      "                  if using QEMU, just use -c 0.\n\n"

      "Fuzzing behavior settings:\n"
      "  -N            - do not unlink the fuzzing input file (for devices "
      "etc.)\n"
      "  -d            - quick & dirty mode (skips deterministic steps)\n"
      "  -n            - fuzz without instrumentation (non-instrumented mode)\n"
      "  -x dict_file  - optional fuzzer dictionary (see README.md, its really "
      "good!)\n\n"

      "Testing settings:\n"
      "  -s seed       - use a fixed seed for the RNG\n"
      "  -V seconds    - fuzz for a specific time then terminate\n"
      "  -E execs      - fuzz for a approx. no of total executions then "
      "terminate\n"
      "                  Note: not precise and can have several more "
      "executions.\n\n"

      "Other stuff:\n"
      "  -M/-S id      - distributed mode (see docs/parallel_fuzzing.md)\n"
      "                  use -D to force -S secondary to perform deterministic "
      "fuzzing\n"
      "  -F path       - sync to a foreign fuzzer queue directory (requires "
      "-M, can\n"
      "                  be specified up to %u times)\n"
      "  -T text       - text banner to show on the screen\n"
      "  -I command    - execute this command/script when a new crash is "
      "found\n"
      //"  -B bitmap.txt - mutate a specific test case, use the out/fuzz_bitmap
      //" "file\n"
      "  -C            - crash exploration mode (the peruvian rabbit thing)\n"
      "  -b cpu_id     - bind the fuzzing process to the specified CPU core "
      "(0-...)\n"
      "  -e ext        - file extension for the fuzz test input file (if "
      "needed)\n\n",
      argv0, EXEC_TIMEOUT, MEM_LIMIT, FOREIGN_SYNCS_MAX);

  if (more_help > 1) {

    SAYF(
      "Environment variables used:\n"
      "LD_BIND_LAZY: do not set LD_BIND_NOW env var for target\n"
      "ASAN_OPTIONS: custom settings for ASAN\n"
      "              (must contain abort_on_error=1 and symbolize=0)\n"
      "MSAN_OPTIONS: custom settings for MSAN\n"
      "              (must contain exitcode="STRINGIFY(MSAN_ERROR)" and symbolize=0)\n"
      "AFL_AUTORESUME: resume fuzzing if directory specified by -o already exists\n"
      "AFL_BENCH_JUST_ONE: run the target just once\n"
      "AFL_BENCH_UNTIL_CRASH: exit soon when the first crashing input has been found\n"
      "AFL_CUSTOM_MUTATOR_LIBRARY: lib with afl_custom_fuzz() to mutate inputs\n"
      "AFL_CUSTOM_MUTATOR_ONLY: avoid AFL++'s internal mutators\n"
      "AFL_CYCLE_SCHEDULES: after completing a cycle, switch to a different -p schedule\n"
      "AFL_DEBUG: extra debugging output for Python mode trimming\n"
      "AFL_DEBUG_CHILD_OUTPUT: do not suppress stdout/stderr from target\n"
      "AFL_DISABLE_TRIM: disable the trimming of test cases\n"
      "AFL_DUMB_FORKSRV: use fork server without feedback from target\n"
      "AFL_EXIT_WHEN_DONE: exit when all inputs are run and no new finds are found\n"
      "AFL_EXPAND_HAVOC_NOW: immediately enable expand havoc mode (default: after 60 minutes and a cycle without finds)\n"
      "AFL_FAST_CAL: limit the calibration stage to three cycles for speedup\n"
      "AFL_FORCE_UI: force showing the status screen (for virtual consoles)\n"
      "AFL_HANG_TMOUT: override timeout value (in milliseconds)\n"
      "AFL_FORKSRV_INIT_TMOUT: time spent waiting for forkserver during startup (in milliseconds)\n"
      "AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES: don't warn about core dump handlers\n"
      "AFL_IMPORT_FIRST: sync and import test cases from other fuzzer instances first\n"
      "AFL_MAP_SIZE: the shared memory size for that target. must be >= the size\n"
      "              the target was compiled for\n"
      "AFL_MAX_DET_EXTRAS: if more entries are in the dictionary list than this value\n"
      "                    then they are randomly selected instead all of them being\n"
      "                    used. Defaults to 200.\n"
      "AFL_NO_AFFINITY: do not check for an unused cpu core to use for fuzzing\n"
      "AFL_NO_ARITH: skip arithmetic mutations in deterministic stage\n"
      "AFL_NO_CPU_RED: avoid red color for showing very high cpu usage\n"
      "AFL_NO_FORKSRV: run target via execve instead of using the forkserver\n"
      "AFL_NO_SNAPSHOT: do not use the snapshot feature (if the snapshot lkm is loaded)\n"
      "AFL_NO_UI: switch status screen off\n"
      "AFL_PATH: path to AFL support binaries\n"
      "AFL_PYTHON_MODULE: mutate and trim inputs with the specified Python module\n"
      "AFL_QUIET: suppress forkserver status messages\n"
      "AFL_PRELOAD: LD_PRELOAD / DYLD_INSERT_LIBRARIES settings for target\n"
      "AFL_SHUFFLE_QUEUE: reorder the input queue randomly on startup\n"
      "AFL_SKIP_BIN_CHECK: skip the check, if the target is an executable\n"
      "AFL_SKIP_CPUFREQ: do not warn about variable cpu clocking\n"
      "AFL_SKIP_CRASHES: during initial dry run do not terminate for crashing inputs\n"
      "AFL_TMPDIR: directory to use for input file generation (ramdisk recommended)\n"
      //"AFL_PERSISTENT: not supported anymore -> no effect, just a warning\n"
      //"AFL_DEFER_FORKSRV: not supported anymore -> no effect, just a warning\n"
      "\n"
    );

  } else {

    SAYF(
        "To view also the supported environment variables of afl-fuzz please "
        "use \"-hh\".\n\n");

  }

#ifdef USE_PYTHON
  SAYF("Compiled with %s module support, see docs/custom_mutator.md\n",
       (char *)PYTHON_VERSION);
#else
  SAYF("Compiled without python module support\n");
#endif

  SAYF("For additional help please consult %s/README.md\n\n", doc_path);

  exit(1);
#undef PHYTON_SUPPORT

}

#ifndef AFL_LIB

static int stricmp(char const *a, char const *b) {

  if (!a || !b) { FATAL("Null reference"); }

  for (;; ++a, ++b) {

    int d;
    d = tolower((int)*a) - tolower((int)*b);
    if (d != 0 || !*a) { return d; }

  }

}

/* Main entry point */

int main(int argc, char **argv_orig, char **envp) {

  s32    opt;
  u64    prev_queued = 0;
  u32    sync_interval_cnt = 0, seek_to, show_help = 0, map_size = MAP_SIZE;
  u8 *   extras_dir = 0;
  u8     mem_limit_given = 0, exit_1 = 0, debug = 0;
  char **use_argv;

  struct timeval  tv;
  struct timezone tz;

  char **argv = argv_cpy_dup(argc, argv_orig);

  afl_state_t *afl = calloc(1, sizeof(afl_state_t));
  if (!afl) { FATAL("Could not create afl state"); }

  if (get_afl_env("AFL_DEBUG")) { debug = afl->debug = 1; }

  map_size = get_map_size();
  afl_state_init(afl, map_size);
  afl->debug = debug;
  afl_fsrv_init(&afl->fsrv);

  u32 tree_dummy[4];
  if(afl->n_cov_level == 1) {
    afl->n_tree_nodes = afl->n_tree_fuzzed_nodes = afl->n_tree_bits = afl->n_tree_fuzzed_bits = tree_dummy;
  }

  read_afl_environment(afl, envp);
  if (afl->shm.map_size) { afl->fsrv.map_size = afl->shm.map_size; }
  exit_1 = !!afl->afl_env.afl_bench_just_one;


  SAYF(cCYA "afl-fuzz" VERSION cRST
            " based on afl by Michal Zalewski and a big online community\n");

  doc_path = access(DOC_PATH, F_OK) != 0 ? (u8 *)"docs" : (u8 *)DOC_PATH;

  gettimeofday(&tv, &tz);
  rand_set_seed(afl, tv.tv_sec ^ tv.tv_usec ^ getpid());

  afl->shmem_testcase_mode = 1;  // we always try to perform shmem fuzzing

  while ((opt = getopt(
              argc, argv,
              "+b:c:i:I:o:f:F:m:t:T:dDnCB:S:M:x:QNUWe:p:s:V:E:L:hRP:")) > 0) {

    switch (opt) {

      case 'I':
        afl->infoexec = optarg;
        break;

      case 'b': {                                          /* bind CPU core */

        if (afl->cpu_to_bind != -1) FATAL("Multiple -b options not supported");

        if (sscanf(optarg, "%d", &afl->cpu_to_bind) < 0) {

          FATAL("Bad syntax used for -b");

        }

        break;

      }

      case 'c': {

        afl->shm.cmplog_mode = 1;
        afl->cmplog_binary = ck_strdup(optarg);
        break;

      }

      case 's': {

        rand_set_seed(afl, strtoul(optarg, 0L, 10));
        afl->fixed_seed = 1;
        break;

      }

      case 'p':                                           /* Power schedule */

        if (!stricmp(optarg, "fast")) {

          afl->schedule = FAST;

        } else if (!stricmp(optarg, "coe")) {

          afl->schedule = COE;

        } else if (!stricmp(optarg, "exploit")) {

          afl->schedule = EXPLOIT;

        } else if (!stricmp(optarg, "lin")) {

          afl->schedule = LIN;

        } else if (!stricmp(optarg, "quad")) {

          afl->schedule = QUAD;

        } else if (!stricmp(optarg, "mopt") || !stricmp(optarg, "mmopt")) {

          afl->schedule = MMOPT;

        } else if (!stricmp(optarg, "rare")) {

          afl->schedule = RARE;

        } else if (!stricmp(optarg, "seek")) {

          afl->schedule = SEEK;

        } else if (!stricmp(optarg, "explore") || !stricmp(optarg, "default") ||

                   !stricmp(optarg, "normal") || !stricmp(optarg, "afl")) {

          afl->schedule = EXPLORE;

        } else {

          FATAL("Unknown -p power schedule");

        }

        break;

      case 'e':

        if (afl->file_extension) { FATAL("Multiple -e options not supported"); }

        afl->file_extension = optarg;

        break;

      case 'i':                                                /* input dir */

        if (afl->in_dir) { FATAL("Multiple -i options not supported"); }
        afl->in_dir = optarg;

        if (!strcmp(afl->in_dir, "-")) { afl->in_place_resume = 1; }

        break;

      case 'o':                                               /* output dir */

        if (afl->out_dir) { FATAL("Multiple -o options not supported"); }
        afl->out_dir = optarg;
        break;

      case 'M': {                                           /* main sync ID */

        u8 *c;

        if (afl->sync_id) { FATAL("Multiple -S or -M options not supported"); }
        afl->sync_id = ck_strdup(optarg);

        if ((c = strchr(afl->sync_id, ':'))) {

          *c = 0;

          if (sscanf(c + 1, "%u/%u", &afl->main_node_id, &afl->main_node_max) !=
                  2 ||
              !afl->main_node_id || !afl->main_node_max ||
              afl->main_node_id > afl->main_node_max ||
              afl->main_node_max > 1000000) {

            FATAL("Bogus main node ID passed to -M");

          }

        }

        afl->is_main_node = 1;

      }

      break;

      case 'S':                                        /* secondary sync id */

        if (afl->sync_id) { FATAL("Multiple -S or -M options not supported"); }
        afl->sync_id = ck_strdup(optarg);
        afl->is_secondary_node = 1;
        afl->skip_deterministic = 1;
        afl->use_splicing = 1;
        break;

      case 'F':                                         /* foreign sync dir */

        if (!afl->is_main_node)
          FATAL(
              "Option -F can only be specified after the -M option for the "
              "main fuzzer of a fuzzing campaign");
        if (afl->foreign_sync_cnt >= FOREIGN_SYNCS_MAX)
          FATAL("Maximum %u entried of -F option can be specified",
                FOREIGN_SYNCS_MAX);
        afl->foreign_syncs[afl->foreign_sync_cnt].dir = optarg;
        afl->foreign_sync_cnt++;
        break;

      case 'f':                                              /* target file */

        if (afl->fsrv.out_file) { FATAL("Multiple -f options not supported"); }
        afl->fsrv.out_file = ck_strdup(optarg);
        afl->fsrv.use_stdin = 0;
        break;

      case 'x':                                               /* dictionary */

        if (extras_dir) { FATAL("Multiple -x options not supported"); }
        extras_dir = optarg;
        break;

      case 't': {                                                /* timeout */

        u8 suffix = 0;

        if (afl->timeout_given) { FATAL("Multiple -t options not supported"); }

        if (sscanf(optarg, "%u%c", &afl->fsrv.exec_tmout, &suffix) < 1 ||
            optarg[0] == '-') {

          FATAL("Bad syntax used for -t");

        }

        if (afl->fsrv.exec_tmout < 5) { FATAL("Dangerously low value of -t"); }

        if (suffix == '+') {

          afl->timeout_given = 2;

        } else {

          afl->timeout_given = 1;

        }

        break;

      }

      case 'm': {                                              /* mem limit */

        u8 suffix = 'M';

        if (mem_limit_given) { FATAL("Multiple -m options not supported"); }
        mem_limit_given = 1;

        if (!optarg) { FATAL("Wrong usage of -m"); }

        if (!strcmp(optarg, "none")) {

          afl->fsrv.mem_limit = 0;
          break;

        }

        if (sscanf(optarg, "%llu%c", &afl->fsrv.mem_limit, &suffix) < 1 ||
            optarg[0] == '-') {

          FATAL("Bad syntax used for -m");

        }

        switch (suffix) {

          case 'T':
            afl->fsrv.mem_limit *= 1024 * 1024;
            break;
          case 'G':
            afl->fsrv.mem_limit *= 1024;
            break;
          case 'k':
            afl->fsrv.mem_limit /= 1024;
            break;
          case 'M':
            break;

          default:
            FATAL("Unsupported suffix or bad syntax for -m");

        }

        if (afl->fsrv.mem_limit < 5) { FATAL("Dangerously low value of -m"); }

        if (sizeof(rlim_t) == 4 && afl->fsrv.mem_limit > 2000) {

          FATAL("Value of -m out of range on 32-bit systems");

        }

      }

      break;

      case 'D':                                    /* enforce deterministic */

        afl->skip_deterministic = 0;
        break;

      case 'd':                                       /* skip deterministic */

        afl->skip_deterministic = 1;
        afl->use_splicing = 1;
        break;

      case 'B':                                              /* load bitmap */

        /* This is a secret undocumented option! It is useful if you find
           an interesting test case during a normal fuzzing process, and want
           to mutate it without rediscovering any of the test cases already
           found during an earlier run.

           To use this mode, you need to point -B to the fuzz_bitmap produced
           by an earlier run for the exact same binary... and that's it.

           I only used this once or twice to get variants of a particular
           file, so I'm not making this an official setting. */

        if (afl->in_bitmap) { FATAL("Multiple -B options not supported"); }

        afl->in_bitmap = optarg;
        read_bitmap(afl->in_bitmap, afl->virgin_bits, afl->fsrv.map_size * afl->n_cov_level);
        break;

      case 'C':                                               /* crash mode */

        if (afl->crash_mode) { FATAL("Multiple -C options not supported"); }
        afl->crash_mode = FSRV_RUN_CRASH;
        break;

      case 'n':                                                /* dumb mode */

        if (afl->non_instrumented_mode) {

          FATAL("Multiple -n options not supported");

        }

        if (afl->afl_env.afl_dumb_forksrv) {

          afl->non_instrumented_mode = 2;

        } else {

          afl->non_instrumented_mode = 1;

        }

        break;

      case 'T':                                                   /* banner */

        if (afl->use_banner) { FATAL("Multiple -T options not supported"); }
        afl->use_banner = optarg;
        break;

      case 'Q':                                                /* QEMU mode */

        if (afl->fsrv.qemu_mode) { FATAL("Multiple -Q options not supported"); }
        afl->fsrv.qemu_mode = 1;

        if (!mem_limit_given) { afl->fsrv.mem_limit = MEM_LIMIT_QEMU; }

        break;

      case 'N':                                             /* Unicorn mode */

        if (afl->no_unlink) { FATAL("Multiple -N options not supported"); }
        afl->no_unlink = 1;

        break;

      case 'U':                                             /* Unicorn mode */

        if (afl->unicorn_mode) { FATAL("Multiple -U options not supported"); }
        afl->unicorn_mode = 1;

        if (!mem_limit_given) { afl->fsrv.mem_limit = MEM_LIMIT_UNICORN; }

        break;

      case 'W':                                           /* Wine+QEMU mode */

        if (afl->use_wine) { FATAL("Multiple -W options not supported"); }
        afl->fsrv.qemu_mode = 1;
        afl->use_wine = 1;

        if (!mem_limit_given) { afl->fsrv.mem_limit = 0; }

        break;

      case 'V': {

        afl->most_time_key = 1;
        if (sscanf(optarg, "%llu", &afl->most_time) < 1 || optarg[0] == '-') {

          FATAL("Bad syntax used for -V");

        }

      } break;

      case 'E': {

        afl->most_execs_key = 1;
        if (sscanf(optarg, "%llu", &afl->most_execs) < 1 || optarg[0] == '-') {

          FATAL("Bad syntax used for -E");

        }

      } break;

      case 'L': {                                              /* MOpt mode */

        if (afl->limit_time_sig) { FATAL("Multiple -L options not supported"); }
        afl->havoc_max_mult = HAVOC_MAX_MULT_MOPT;

        if (sscanf(optarg, "%d", &afl->limit_time_puppet) < 1) {

          FATAL("Bad syntax used for -L");

        }

        if (afl->limit_time_puppet == -1) {

          afl->limit_time_sig = -1;
          afl->limit_time_puppet = 0;

        } else if (afl->limit_time_puppet < 0) {

          FATAL("-L value must be between 0 and 2000000 or -1");

        } else {

          afl->limit_time_sig = 1;

        }

        u64 limit_time_puppet2 = afl->limit_time_puppet * 60 * 1000;

        if ((s32)limit_time_puppet2 < afl->limit_time_puppet) {

          FATAL("limit_time overflow");

        }

        afl->limit_time_puppet = limit_time_puppet2;
        afl->swarm_now = 0;
        if (afl->limit_time_puppet == 0) { afl->key_puppet = 1; }

        int i;
        int tmp_swarm = 0;

        if (afl->g_now > afl->g_max) { afl->g_now = 0; }
        afl->w_now = (afl->w_init - afl->w_end) * (afl->g_max - afl->g_now) /
                         (afl->g_max) +
                     afl->w_end;

        for (tmp_swarm = 0; tmp_swarm < swarm_num; ++tmp_swarm) {

          double total_puppet_temp = 0.0;
          afl->swarm_fitness[tmp_swarm] = 0.0;

          for (i = 0; i < operator_num; ++i) {

            afl->stage_finds_puppet[tmp_swarm][i] = 0;
            afl->probability_now[tmp_swarm][i] = 0.0;
            afl->x_now[tmp_swarm][i] =
                ((double)(random() % 7000) * 0.0001 + 0.1);
            total_puppet_temp += afl->x_now[tmp_swarm][i];
            afl->v_now[tmp_swarm][i] = 0.1;
            afl->L_best[tmp_swarm][i] = 0.5;
            afl->G_best[i] = 0.5;
            afl->eff_best[tmp_swarm][i] = 0.0;

          }

          for (i = 0; i < operator_num; ++i) {

            afl->stage_cycles_puppet_v2[tmp_swarm][i] =
                afl->stage_cycles_puppet[tmp_swarm][i];
            afl->stage_finds_puppet_v2[tmp_swarm][i] =
                afl->stage_finds_puppet[tmp_swarm][i];
            afl->x_now[tmp_swarm][i] =
                afl->x_now[tmp_swarm][i] / total_puppet_temp;

          }

          double x_temp = 0.0;

          for (i = 0; i < operator_num; ++i) {

            afl->probability_now[tmp_swarm][i] = 0.0;
            afl->v_now[tmp_swarm][i] =
                afl->w_now * afl->v_now[tmp_swarm][i] +
                RAND_C *
                    (afl->L_best[tmp_swarm][i] - afl->x_now[tmp_swarm][i]) +
                RAND_C * (afl->G_best[i] - afl->x_now[tmp_swarm][i]);

            afl->x_now[tmp_swarm][i] += afl->v_now[tmp_swarm][i];

            if (afl->x_now[tmp_swarm][i] > v_max) {

              afl->x_now[tmp_swarm][i] = v_max;

            } else if (afl->x_now[tmp_swarm][i] < v_min) {

              afl->x_now[tmp_swarm][i] = v_min;

            }

            x_temp += afl->x_now[tmp_swarm][i];

          }

          for (i = 0; i < operator_num; ++i) {

            afl->x_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i] / x_temp;
            if (likely(i != 0)) {

              afl->probability_now[tmp_swarm][i] =
                  afl->probability_now[tmp_swarm][i - 1] +
                  afl->x_now[tmp_swarm][i];

            } else {

              afl->probability_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i];

            }

          }

          if (afl->probability_now[tmp_swarm][operator_num - 1] < 0.99 ||
              afl->probability_now[tmp_swarm][operator_num - 1] > 1.01) {

            FATAL("ERROR probability");

          }

        }

        for (i = 0; i < operator_num; ++i) {

          afl->core_operator_finds_puppet[i] = 0;
          afl->core_operator_finds_puppet_v2[i] = 0;
          afl->core_operator_cycles_puppet[i] = 0;
          afl->core_operator_cycles_puppet_v2[i] = 0;
          afl->core_operator_cycles_puppet_v3[i] = 0;

        }

      } break;

      case 'h':
        show_help++;
        break;  // not needed

      case 'R':

        FATAL(
            "Radamsa is now a custom mutator, please use that "
            "(custom_mutators/radamsa/).");

        break;

      default:
        if (!show_help) { show_help = 1; }

    }

  }

  if (optind == argc || !afl->in_dir || !afl->out_dir || show_help) {

    usage(argv[0], show_help);

  }

  if (!mem_limit_given && afl->shm.cmplog_mode) afl->fsrv.mem_limit += 260;

  OKF("afl++ is maintained by Marc \"van Hauser\" Heuse, Heiko \"hexcoder\" "
      "Eißfeldt, Andrea Fioraldi and Dominik Maier");
  OKF("afl++ is open source, get it at "
      "https://github.com/AFLplusplus/AFLplusplus");
  OKF("Power schedules from github.com/mboehme/aflfast");
  OKF("Python Mutator and llvm_mode instrument file list from "
      "github.com/choller/afl");
  OKF("MOpt Mutator from github.com/puppet-meteor/MOpt-AFL");

  if(afl->n_cov_level > 1) OKF("use multi-level coverage");
  if(afl->use_hier_sched) OKF("use hierarchical seed scheduler");

  if (afl->sync_id && afl->is_main_node &&
      afl->afl_env.afl_custom_mutator_only) {

    WARNF(
        "Using -M main node with the AFL_CUSTOM_MUTATOR_ONLY mutator options "
        "will result in no deterministic mutations being done!");

  }

  if (afl->fixed_seed) {

    OKF("Running with fixed seed: %u", (u32)afl->init_seed);

  }

  #if defined(__SANITIZE_ADDRESS__)
  if (afl->fsrv.mem_limit) {

    WARNF("in the ASAN build we disable all memory limits");
    afl->fsrv.mem_limit = 0;

  }

  #endif

  setup_signal_handlers();
  check_asan_opts();

  afl->power_name = power_names[afl->schedule];

  if (afl->sync_id) { fix_up_sync(afl); }

  if (!strcmp(afl->in_dir, afl->out_dir)) {

    FATAL("Input and output directories can't be the same");

  }

  if (afl->non_instrumented_mode) {

    if (afl->crash_mode) { FATAL("-C and -n are mutually exclusive"); }
    if (afl->fsrv.qemu_mode) { FATAL("-Q and -n are mutually exclusive"); }
    if (afl->unicorn_mode) { FATAL("-U and -n are mutually exclusive"); }

  }

  if (get_afl_env("AFL_DISABLE_TRIM")) { afl->disable_trim = 1; }

  if (getenv("AFL_NO_UI") && getenv("AFL_FORCE_UI")) {

    FATAL("AFL_NO_UI and AFL_FORCE_UI are mutually exclusive");

  }

  if (strchr(argv[optind], '/') == NULL && !afl->unicorn_mode) {

    WARNF(cLRD
          "Target binary called without a prefixed path, make sure you are "
          "fuzzing the right binary: " cRST "%s",
          argv[optind]);

  }

  ACTF("Getting to work...");

  switch (afl->schedule) {

    case FAST:
      OKF("Using exponential power schedule (FAST)");
      break;
    case COE:
      OKF("Using cut-off exponential power schedule (COE)");
      break;
    case EXPLOIT:
      OKF("Using exploitation-based constant power schedule (EXPLOIT)");
      break;
    case LIN:
      OKF("Using linear power schedule (LIN)");
      break;
    case QUAD:
      OKF("Using quadratic power schedule (QUAD)");
      break;
    case MMOPT:
      OKF("Using modified MOpt power schedule (MMOPT)");
      break;
    case RARE:
      OKF("Using rare edge focus power schedule (RARE)");
      break;
    case SEEK:
      OKF("Using seek power schedule (SEEK)");
      break;
    case EXPLORE:
      OKF("Using exploration-based constant power schedule (EXPLORE, default)");
      break;
    default:
      FATAL("Unknown power schedule");
      break;

  }

  if (get_afl_env("AFL_NO_FORKSRV")) { afl->no_forkserver = 1; }
  if (get_afl_env("AFL_NO_CPU_RED")) { afl->no_cpu_meter_red = 1; }
  if (get_afl_env("AFL_NO_ARITH")) { afl->no_arith = 1; }
  if (get_afl_env("AFL_SHUFFLE_QUEUE")) { afl->shuffle_queue = 1; }
  if (get_afl_env("AFL_FAST_CAL")) { afl->fast_cal = 1; }
  if (get_afl_env("AFL_EXPAND_HAVOC_NOW")) { afl->expand_havoc = 1; }

  if (afl->afl_env.afl_autoresume) {

    afl->autoresume = 1;
    if (afl->in_place_resume) {

      SAYF("AFL_AUTORESUME has no effect for '-i -'");

    }

  }

  if (afl->afl_env.afl_hang_tmout) {

    s32 hang_tmout = atoi(afl->afl_env.afl_hang_tmout);
    if (hang_tmout < 1) { FATAL("Invalid value for AFL_HANG_TMOUT"); }
    afl->hang_tmout = (u32)hang_tmout;

  }

  if (afl->afl_env.afl_max_det_extras) {

    s32 max_det_extras = atoi(afl->afl_env.afl_max_det_extras);
    if (max_det_extras < 1) { FATAL("Invalid value for AFL_MAX_DET_EXTRAS"); }
    afl->max_det_extras = (u32)max_det_extras;

  } else {

    afl->max_det_extras = MAX_DET_EXTRAS;

  }

  if (afl->afl_env.afl_forksrv_init_tmout) {

    afl->fsrv.init_tmout = atoi(afl->afl_env.afl_forksrv_init_tmout);
    if (!afl->fsrv.init_tmout) {

      FATAL("Invalid value of AFL_FORKSRV_INIT_TMOUT");

    }

  } else {

    afl->fsrv.init_tmout = afl->fsrv.exec_tmout * FORK_WAIT_MULT;

  }

  if (afl->non_instrumented_mode == 2 && afl->no_forkserver) {

    FATAL("AFL_DUMB_FORKSRV and AFL_NO_FORKSRV are mutually exclusive");

  }

  afl->fsrv.use_fauxsrv = afl->non_instrumented_mode == 1 || afl->no_forkserver;

  if (getenv("LD_PRELOAD")) {

    WARNF(
        "LD_PRELOAD is set, are you sure that is what to you want to do "
        "instead of using AFL_PRELOAD?");

  }

  if (afl->afl_env.afl_preload) {

    if (afl->fsrv.qemu_mode) {

      u8 *qemu_preload = getenv("QEMU_SET_ENV");
      u8 *afl_preload = getenv("AFL_PRELOAD");
      u8 *buf;

      s32 i, afl_preload_size = strlen(afl_preload);
      for (i = 0; i < afl_preload_size; ++i) {

        if (afl_preload[i] == ',') {

          PFATAL(
              "Comma (',') is not allowed in AFL_PRELOAD when -Q is "
              "specified!");

        }

      }

      if (qemu_preload) {

        buf = alloc_printf("%s,LD_PRELOAD=%s,DYLD_INSERT_LIBRARIES=%s",
                           qemu_preload, afl_preload, afl_preload);

      } else {

        buf = alloc_printf("LD_PRELOAD=%s,DYLD_INSERT_LIBRARIES=%s",
                           afl_preload, afl_preload);

      }

      setenv("QEMU_SET_ENV", buf, 1);

      ck_free(buf);

    } else {

      setenv("LD_PRELOAD", getenv("AFL_PRELOAD"), 1);
      setenv("DYLD_INSERT_LIBRARIES", getenv("AFL_PRELOAD"), 1);

    }

  }

  if (getenv("AFL_LD_PRELOAD")) {

    FATAL("Use AFL_PRELOAD instead of AFL_LD_PRELOAD");

  }

  save_cmdline(afl, argc, argv);

  fix_up_banner(afl, argv[optind]);

  check_if_tty(afl);
  if (afl->afl_env.afl_force_ui) { afl->not_on_tty = 0; }

  if (afl->afl_env.afl_cal_fast) {

    /* Use less calibration cycles, for slow applications */
    afl->cal_cycles = 3;
    afl->cal_cycles_long = 5;

  }

  if (afl->afl_env.afl_custom_mutator_only) {

    /* This ensures we don't proceed to havoc/splice */
    afl->custom_only = 1;

    /* Ensure we also skip all deterministic steps */
    afl->skip_deterministic = 1;

  }

  check_crash_handling();
  check_cpu_governor(afl);

  get_core_count(afl);

  atexit(at_exit);

  setup_dirs_fds(afl);

  #ifdef HAVE_AFFINITY
  bind_to_free_cpu(afl);
  #endif                                                   /* HAVE_AFFINITY */

  #ifdef __HAIKU__
  /* Prioritizes performance over power saving */
  set_scheduler_mode(SCHEDULER_MODE_LOW_LATENCY);
  #endif

  afl->fsrv.trace_ctrl =
      afl_shm_init(&afl->shm, afl->fsrv.map_size * afl->n_cov_level + 1, afl->non_instrumented_mode);
      
  afl->fsrv.trace_bits_core = afl->fsrv.trace_bits = afl->fsrv.trace_ctrl + 1;

  afl->virgin_bits_core = afl->virgin_bits;

  if(afl->n_cov_level > 1) {
      
      afl->fsrv.trace_bits_core += afl->fsrv.map_size * (COV_EDGE - 1);

      afl->virgin_bits_core += afl->fsrv.map_size * (COV_EDGE - 1);

  }

  // OKF("trace_bits: 0x%x, _core: 0x%x\n", afl->fsrv.trace_bits, afl->fsrv.trace_bits_core);

  if (!afl->in_bitmap) { memset(afl->virgin_bits, 255, afl->fsrv.map_size * afl->n_cov_level); }
  memset(afl->virgin_tmout, 255, afl->fsrv.map_size);
  memset(afl->virgin_crash, 255, afl->fsrv.map_size);

  init_count_class16();

  if (afl->is_main_node && check_main_node_exists(afl) == 1) {

    WARNF("it is wasteful to run more than one main node!");
    sleep(1);

  }

  if (afl->is_secondary_node && check_main_node_exists(afl) == 0) {

    WARNF("no -M main node found. You need to run one main instance!");
    sleep(3);

  }

  #ifdef RAND_TEST_VALUES
  u32 counter;
  for (counter = 0; counter < 100000; counter++)
    printf("DEBUG: rand %06d is %u\n", counter, rand_below(afl, 65536));
  #endif

  setup_custom_mutators(afl);

  write_setup_file(afl, argc, argv);

  setup_cmdline_file(afl, argv + optind);

  read_testcases(afl);
  // read_foreign_testcases(afl, 1); for the moment dont do this

  load_auto(afl);

  pivot_inputs(afl);

  if (extras_dir) { load_extras(afl, extras_dir); }

  if (!afl->timeout_given) { find_timeout(afl); }

  if ((afl->tmp_dir = afl->afl_env.afl_tmpdir) != NULL &&
      !afl->in_place_resume) {

    char tmpfile[PATH_MAX];

    if (afl->file_extension) {

      snprintf(tmpfile, PATH_MAX, "%s/.cur_input.%s", afl->tmp_dir,
               afl->file_extension);

    } else {

      snprintf(tmpfile, PATH_MAX, "%s/.cur_input", afl->tmp_dir);

    }

    /* there is still a race condition here, but well ... */
    if (access(tmpfile, F_OK) != -1) {

      FATAL(
          "AFL_TMPDIR already has an existing temporary input file: %s - if "
          "this is not from another instance, then just remove the file.",
          tmpfile);

    }

  } else {

    afl->tmp_dir = afl->out_dir;

  }

  /* If we don't have a file name chosen yet, use a safe default. */

  if (!afl->fsrv.out_file) {

    u32 i = optind + 1;
    while (argv[i]) {

      u8 *aa_loc = strstr(argv[i], "@@");

      if (aa_loc && !afl->fsrv.out_file) {

        afl->fsrv.use_stdin = 0;

        if (afl->file_extension) {

          afl->fsrv.out_file = alloc_printf("%s/.cur_input.%s", afl->tmp_dir,
                                            afl->file_extension);

        } else {

          afl->fsrv.out_file = alloc_printf("%s/.cur_input", afl->tmp_dir);

        }

        detect_file_args(argv + optind + 1, afl->fsrv.out_file,
                         &afl->fsrv.use_stdin);
        break;

      }

      ++i;

    }

  }

  if (!afl->fsrv.out_file) { setup_stdio_file(afl); }

  if (afl->cmplog_binary) {

    if (afl->unicorn_mode) {

      FATAL("CmpLog and Unicorn mode are not compatible at the moment, sorry");

    }

    if (!afl->fsrv.qemu_mode) { check_binary(afl, afl->cmplog_binary); }

  }

  check_binary(afl, argv[optind]);

  if (afl->shmem_testcase_mode) { setup_testcase_shmem(afl); }

  afl->start_time = get_cur_time();

  if (afl->fsrv.qemu_mode) {

    if (afl->use_wine) {

      use_argv = get_wine_argv(argv[0], &afl->fsrv.target_path, argc - optind,
                               argv + optind);

    } else {

      use_argv = get_qemu_argv(argv[0], &afl->fsrv.target_path, argc - optind,
                               argv + optind);

    }

  } else {

    use_argv = argv + optind;

  }

  afl->argv = use_argv;

  if (afl->cmplog_binary) {

    ACTF("Spawning cmplog forkserver");
    afl_fsrv_init_dup(&afl->cmplog_fsrv, &afl->fsrv);
    // TODO: this is semi-nice
    afl->cmplog_fsrv.trace_bits = afl->fsrv.trace_bits_core;
    afl->cmplog_fsrv.qemu_mode = afl->fsrv.qemu_mode;
    afl->cmplog_fsrv.cmplog_binary = afl->cmplog_binary;
    afl->cmplog_fsrv.init_child_func = cmplog_exec_child;
    afl_fsrv_start(&afl->cmplog_fsrv, afl->argv, &afl->stop_soon,
                   afl->afl_env.afl_debug_child_output);
    OKF("Cmplog forkserver successfully started");

  }

  perform_dry_run(afl);

  // goto stop_fuzzing;

  cull_queue(afl);

  show_init_stats(afl);

  seek_to = find_start_position(afl);

  write_stats_file(afl, 0, 0, 0);
  maybe_update_plot_file(afl, 0, 0);
  save_auto(afl);

  if (afl->stop_soon) { goto stop_fuzzing; }

  /* Woop woop woop */

  if (!afl->not_on_tty) {

    sleep(4);
    afl->start_time += 4000;
    if (afl->stop_soon) { goto stop_fuzzing; }

  }

  // (void)nice(-20);  // does not improve the speed
  // real start time, we reset, so this works correctly with -V
  afl->start_time = get_cur_time();
   
#ifdef CALCULATE_OVERHEAD
    double T0 = get_timestamp();
    double overhead = 0.0;

    u8 *scheduler_overhead_csv_file_name = alloc_printf("%s/scheduler_overhead.csv", afl->out_dir);
    s32 fd = open(scheduler_overhead_csv_file_name, O_WRONLY | O_APPEND | O_CREAT, DEFAULT_PERMISSION);
    if (unlikely(fd < 0)) { PFATAL("Unable to create %s/scheduler_overhead.csv'", afl->out_dir); }
    timestamp_t t0, t1;
    u8 *scheduler_overhead;
;
#endif
   
  while (1) {

#ifdef CALCULATE_OVERHEAD
    t0 = get_timestamp();
#endif
     
    u8 skipped_fuzz;

    if(afl->use_hier_sched){

      update_wo_finds(afl->hier_sched, afl->queued_paths == prev_queued);

      afl->queue_cur = choose_next_seed(afl->hier_sched);
      afl->current_entry = afl->hier_sched->current_entry;
      afl->queue_cycle = afl->hier_sched->current_queue_cycle;
      afl->cur_skipped_paths = 0;

      if ((get_cur_time() - afl->start_time) >= 3600 && (afl->hier_sched->current_wo_finds + 4) % 8 == 0) {

        if (afl->use_splicing) {

          ++afl->cycles_wo_finds;
          switch (afl->expand_havoc) {

            case 0:
              afl->expand_havoc = 1;
              break;
            case 1:
              if (afl->limit_time_sig == 0 && !afl->custom_only &&
                  !afl->python_only) {

                afl->limit_time_sig = -1;
                afl->limit_time_puppet = 0;
                // FATAL("catch it!");
              }

              afl->expand_havoc = 2;
              break;
            case 2:
              // afl->cycle_schedules = 1;
              afl->expand_havoc = 3;
              break;
            case 3:
              // nothing else currently
              break;

          }

          if (afl->expand_havoc) {

          } else

            afl->expand_havoc = 1;

        } else {

          afl->use_splicing = 1;

        }

      } else {

          afl->cycles_wo_finds = 0;

      }

      prev_queued = afl->queued_paths;

    }
    else{
      cull_queue(afl);

      if (!afl->queue_cur) {

        ++afl->queue_cycle;
        afl->current_entry = 0;
        afl->cur_skipped_paths = 0;
        afl->queue_cur = afl->queue;

        while (seek_to) {

          ++afl->current_entry;
          --seek_to;
          afl->queue_cur = afl->queue_cur->next;

        }

        // show_stats(afl);

        if (unlikely(afl->not_on_tty)) {

          ACTF("Entering queue cycle %llu.", afl->queue_cycle);
          fflush(stdout);

        }

        /* If we had a full queue cycle with no new finds, try
          recombination strategies next. */

        if (afl->queued_paths == prev_queued &&
            (get_cur_time() - afl->start_time) >= 3600) {

          if (afl->use_splicing) {

            ++afl->cycles_wo_finds;
            switch (afl->expand_havoc) {

              case 0:
                afl->expand_havoc = 1;
                break;
              case 1:
                if (afl->limit_time_sig == 0 && !afl->custom_only &&
                    !afl->python_only) {

                  afl->limit_time_sig = -1;
                  afl->limit_time_puppet = 0;

                }

                afl->expand_havoc = 2;
                break;
              case 2:
                // afl->cycle_schedules = 1;
                afl->expand_havoc = 3;
                break;
              case 3:
                // nothing else currently
                break;

            }

            if (afl->expand_havoc) {

            } else

              afl->expand_havoc = 1;

          } else {

            afl->use_splicing = 1;

          }

        } else {

          afl->cycles_wo_finds = 0;

        }

        if (afl->cycle_schedules) {

          /* we cannot mix non-AFLfast schedules with others */

          switch (afl->schedule) {

            case EXPLORE:
              afl->schedule = EXPLOIT;
              break;
            case EXPLOIT:
              afl->schedule = MMOPT;
              break;
            case MMOPT:
              afl->schedule = SEEK;
              break;
            case SEEK:
              afl->schedule = EXPLORE;
              break;
            case FAST:
              afl->schedule = COE;
              break;
            case COE:
              afl->schedule = LIN;
              break;
            case LIN:
              afl->schedule = QUAD;
              break;
            case QUAD:
              afl->schedule = RARE;
              break;
            case RARE:
              afl->schedule = FAST;
              break;

          }

          struct queue_entry *q = afl->queue;
          // we must recalculate the scores of all queue entries
          while (q) {

            update_bitmap_score(afl, q);
            q = q->next;

          }

        }

        prev_queued = afl->queued_paths;

        if (afl->sync_id && afl->queue_cycle == 1 &&
            afl->afl_env.afl_import_first) {

          sync_fuzzers(afl);

        }

      }
    }
#ifdef CALCULATE_OVERHEAD
    t1 = get_timestamp();
    overhead += (t1 - t0) / 1000000.0;
    OKF("Seed scehduler overhead is: %.4g, %.4g", overhead / ((t1 - T0) / 1000000.0), afl->update_overhead_sec );
     


    scheduler_overhead = alloc_printf("%f, %f, %f\n", (double) (t1 - T0) / 1000000.0, (double) overhead, afl->update_overhead_sec );

    write((int) (fd), scheduler_overhead, strlen(scheduler_overhead));
#endif

    skipped_fuzz = fuzz_one(afl);

    if (!skipped_fuzz && !afl->stop_soon && afl->sync_id) {

      if (unlikely(afl->is_main_node)) {

        if (!(sync_interval_cnt++ % (SYNC_INTERVAL / 3))) { sync_fuzzers(afl); }

      } else {

        if (!(sync_interval_cnt++ % SYNC_INTERVAL)) { sync_fuzzers(afl); }

      }

    }

    if (!afl->stop_soon && exit_1) { afl->stop_soon = 2; }

    if (afl->stop_soon) { break; }

    if(!afl->use_hier_sched){

      afl->queue_cur = afl->queue_cur->next;
      ++afl->current_entry;

    }

  }

  write_bitmap(afl);
  maybe_update_plot_file(afl, 0, 0);
  save_auto(afl);

stop_fuzzing:

  write_stats_file(afl, 0, 0, 0);
  afl->force_ui_update = 1;  // ensure the screen is reprinted
  show_stats(afl);           // print the screen one last time

  SAYF(CURSOR_SHOW cLRD "\n\n+++ Testing aborted %s +++\n" cRST,
       afl->stop_soon == 2 ? "programmatically" : "by user");

  if (afl->most_time_key == 2) {

    SAYF(cYEL "[!] " cRST "Time limit was reached\n");

  }

  if (afl->most_execs_key == 2) {

    SAYF(cYEL "[!] " cRST "Execution limit was reached\n");

  }

  /* Running for more than 30 minutes but still doing first cycle? */

  if (afl->queue_cycle == 1 &&
      get_cur_time() - afl->start_time > 30 * 60 * 1000) {

    SAYF("\n" cYEL "[!] " cRST
         "Stopped during the first cycle, results may be incomplete.\n"
         "    (For info on resuming, see %s/README.md)\n",
         doc_path);

  }

  #ifdef PROFILING
  SAYF(cYEL "[!] " cRST
            "Profiling information: %llu ms total work, %llu ns/run\n",
       time_spent_working / 1000000,
       time_spent_working / afl->fsrv.total_execs);
  #endif

  if (afl->is_main_node) {

    u8 path[PATH_MAX];
    sprintf(path, "%s/is_main_node", afl->out_dir);
    unlink(path);

  }
   
  #ifdef CALCULATE_OVERHEAD
    close(fd);
    ck_free(scheduler_overhead_csv_file_name);
    ck_free(scheduler_overhead);
#endif


  fclose(afl->fsrv.plot_file);
  destroy_queue(afl);
  destroy_extras(afl);
  destroy_custom_mutators(afl);
  unsetenv(SHM_ENV_VAR);
  unsetenv(CMPLOG_SHM_ENV_VAR);
  afl_shm_deinit(&afl->shm);

  if (afl->shm_fuzz) {

    unsetenv(SHM_FUZZ_ENV_VAR);
    afl_shm_deinit(afl->shm_fuzz);
    ck_free(afl->shm_fuzz);

  }

  afl_fsrv_deinit(&afl->fsrv);
  if (afl->orig_cmdline) { ck_free(afl->orig_cmdline); }
  ck_free(afl->fsrv.target_path);
  ck_free(afl->fsrv.out_file);
  ck_free(afl->sync_id);
  afl_state_deinit(afl);
  free(afl);                                                 /* not tracked */

  argv_cpy_free(argv);

  alloc_report();

  OKF("We're done here. Have a nice day!\n");

  exit(0);

}

#endif                                                          /* !AFL_LIB */

