#ifndef SAMARA_SHELL_COMMANDS_H
#define SAMARA_SHELL_COMMANDS_H
#include "core/types.h"

/* Every builtin's entry point, implemented across shell/commands and wired
 * into the dispatch table in shell.c. */

/* commands_core.c */
void cmd_help(int argc, char **argv);
void cmd_clear(int argc, char **argv);
void cmd_echo(int argc, char **argv);

/* commands_fs.c */
void cmd_pwd(int argc, char **argv);
void cmd_ls(int argc, char **argv);
void cmd_cd(int argc, char **argv);
void cmd_cat(int argc, char **argv);
void cmd_mkdir(int argc, char **argv);
void cmd_touch(int argc, char **argv);
void cmd_rm(int argc, char **argv);
void cmd_cp(int argc, char **argv);
void cmd_mv(int argc, char **argv);
void cmd_head(int argc, char **argv);
void cmd_tail(int argc, char **argv);
void cmd_wc(int argc, char **argv);
void cmd_grep(int argc, char **argv);
void cmd_find(int argc, char **argv);

/* commands_sys.c */
void cmd_uptime(int argc, char **argv);
void cmd_mem(int argc, char **argv);
void cmd_df(int argc, char **argv);
void cmd_ps(int argc, char **argv);
void cmd_mouse(int argc, char **argv);
void cmd_uname(int argc, char **argv);
void cmd_whoami(int argc, char **argv);
void cmd_about(int argc, char **argv);
void cmd_history(int argc, char **argv);
void cmd_exit(int argc, char **argv);
void cmd_sleep(int argc, char **argv);
void cmd_shutdown(int argc, char **argv);
void cmd_reboot(int argc, char **argv);
void cmd_beep(int argc, char **argv);
void cmd_date(int argc, char **argv);
void cmd_calc(int argc, char **argv);
void cmd_neofetch(int argc, char **argv);

/* commands_editor.c */
void cmd_nano(int argc, char **argv);

/* commands_gui.c */
void cmd_desktop(int argc, char **argv);
void cmd_bounce(int argc, char **argv);
void cmd_disk(int argc, char **argv);
void cmd_wallpaper(int argc, char **argv);

/* commands_apps.c */
void cmd_doom_mini(int argc, char **argv);
void cmd_play(int argc, char **argv);
void cmd_doom(int argc, char **argv);
void cmd_snake(int argc, char **argv);
void cmd_player(int argc, char **argv);
void cmd_paint(int argc, char **argv);
void cmd_clock(int argc, char **argv);
void cmd_browser(int argc, char **argv);
void cmd_klayout(int argc, char **argv);

/* commands_media.c */
void cmd_fatmount(int argc, char **argv);
void cmd_fatls(int argc, char **argv);
void cmd_fatload(int argc, char **argv);
void cmd_playfat(int argc, char **argv);
void cmd_playwav(int argc, char **argv);
void cmd_stopwav(int argc, char **argv);
void cmd_sbinfo(int argc, char **argv);

/* commands_net.c */
void cmd_ifconfig(int argc, char **argv);
void cmd_ping(int argc, char **argv);
void cmd_wget(int argc, char **argv);

/* commands_proc.c */
void cmd_sh(int argc, char **argv);
void cmd_run(int argc, char **argv);
void cmd_strace(int argc, char **argv);
void cmd_procs(int argc, char **argv);
void cmd_disks(int argc, char **argv);
void cmd_gnu_nano(int argc, char **argv);
void cmd_python(int argc, char **argv);
void cmd_kbdignore(int argc, char **argv);

/* Ring-3 program launching, shared with shell.c. */
bool shell_find_program(const char *name, char *out, int cap);
int  shell_exec_program(const char *path, int argc, char **argv);
bool shell_fg_running(void);
void shell_fg_key(char c);
bool shell_fg_poll(void);
void shell_fg_abandon(void);

#endif
