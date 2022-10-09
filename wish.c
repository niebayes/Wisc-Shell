#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define MAX_LINE 512
#define MAX_TOKENS 100
#define MAX_PATHS 128

// forward declarations.
void Write(int, const void*, size_t);

// global constants.
const char prompt_str[] = "wish> ";
const char err_msg[] = "An error has occurred\n";
const char default_path[] = "/bin";

// global mutables.
char* path[MAX_PATHS];
int num_path;
char* line;
size_t len;

void init_path() {
  path[0] = malloc(strlen(default_path) + 1);
  strcpy(path[0], default_path);
  num_path++;
}

int append_path(const char* new_path) {
  if (num_path > MAX_PATHS) {
    return -1;
  }
  path[num_path] = malloc(strlen(new_path) + 1);
  memset(path[num_path], 0, strlen(new_path) + 1);  // redundant.
  strcpy(path[num_path], new_path);
  num_path++;
  return 0;
}

void free_path() {
  for (int i = 0; i < MAX_PATHS; i++) {
    if (path[i] != NULL) {
      free(path[i]);
      path[i] = NULL;
    }
  }
  num_path = 0;
}

// error logger.
void log_error() { Write(STDERR_FILENO, err_msg, strlen(err_msg)); }

// syscall wrappers to check return code of each syscall and handle errors
// properly.
void Write(int fd, const void* buf, size_t n) {
  if (write(fd, buf, n) == -1) {
    log_error();
  }
}

void Exit(int rc) {
  if (line != NULL) {
    free(line);
  }
  free_path();
  exit(rc);
}

void Fclose(FILE* stream) {
  if (fclose(stream) == -1) {
    log_error();
  }
}

int execute_group(char** group, int* pids, int* index) {
  int redir_op_index = -1;
  int num_redir_ops = 0;
  int num_redir_files = 0;

  int i = 0;
  for (; group[i] != NULL; i++) {
    if (num_redir_ops > 0) {
      num_redir_files++;
    }

    if (!strcmp(group[i], ">")) {
      redir_op_index = i;
      num_redir_ops++;
    }
  }

  // check user input's validness.
  if (num_redir_ops > 1 || num_redir_files > 1 ||
      (num_redir_ops >= 1 && num_redir_files == 0)) {
    log_error();
    return 0;
  }

  // parse user input.

  // compose args.
  char* args[MAX_TOKENS];
  int num_args = 0;  // exclude the cmd itself.
  char* redir_file = NULL;

  int fd = -1;
  if (redir_op_index == -1) {
    // no redirection requirements.
    for (int j = 1; j < i; j++) {
      args[j] = group[j];
      num_args++;
    }
    args[i] = NULL;

  } else {
    // has redirection requirements.
    for (int j = 1; j < redir_op_index; j++) {
      args[j] = group[j];
      num_args++;
    }
    args[redir_op_index] = NULL;  // terminate args.
    redir_file = group[redir_op_index + 1];

    // if the redirection file exists, check its write permission and check if
    // it's a directory.
    if (access(redir_file, F_OK) != -1) {
      if (access(redir_file, W_OK) == -1) {
        log_error();
        return 0;
      }

      struct stat s;
      if (stat(redir_file, &s) == -1) {
        log_error();
        return 0;
      }
      if (S_ISDIR(s.st_mode)) {
        log_error();
        return 0;
      }

      fd = open(redir_file, O_TRUNC | O_WRONLY);
      if (fd == -1) {
        log_error();
        return 0;
      }

    } else {
      // otherwise, create a new file.
      fd = open(redir_file, O_CREAT | O_WRONLY);
      if (fd == -1) {
        log_error();
        return 0;
      }
    }
  }

  char* cmd = group[0];

  // handle builtin cmds.

  // exit the shell.
  if (!strcmp(cmd, "exit")) {
    if (num_args > 0) {
      log_error();
    }
    return 1;
  }

  // change directory.
  if (!strcmp(cmd, "cd")) {
    if (num_args == 0 || num_args > 1) {
      log_error();
      return 0;
    }
    if (chdir(args[1]) == -1) {
      log_error();
      return 0;
    }
    return 0;
  }

  // add new paths.
  if (!strcmp(cmd, "path")) {
    free_path();
    for (int i = 0; i < num_args; i++) {
      // path array is full.
      if (append_path(args[i + 1]) == -1) {
        log_error();
        return 0;
      }
    }
    return 0;
  }

  // search paths.
  int not_cwd = 0;
  if (access(cmd, X_OK) == -1) {
    not_cwd = 1;
    // if cmd is not in the current directory, search paths.
    int good = 0;
    for (int i = 0; i < num_path; i++) {
      // count on the null terminate char and the link '/'.
      char* cmd0 = malloc(strlen(path[i]) + strlen(cmd) + 2);
      strcpy(cmd0, path[i]);
      cmd0[strlen(path[i])] = '/';
      strcpy(cmd0 + strlen(path[i]) + 1, cmd);
      if (access(cmd0, X_OK) != -1) {
        good = 1;
      }
      if (good) {
        cmd = cmd0;
        break;
      } else {
        free(cmd0);
      }
    }
    if (!good) {
      log_error();
      return 0;
    }
  }

  args[0] = group[0];

  int pid;
  if ((pid = fork()) == 0) {
    // child process.

    // FIXME: properly shutdown redirection file.

    // set redirection if required.
    if (redir_op_index != -1) {
      if (dup2(fd, STDOUT_FILENO) == -1) {
        log_error();
        _exit(1);
      }
    }

    if (execv(cmd, args) == -1) {
      log_error();
      _exit(1);
    }
  } else {
    pids[*index] = pid;
    *index = *index + 1;
    if (not_cwd) {
      free(cmd);
    }
  }

  return 0;
}

///@return 1 if shall terminate the shell.
int execute_line(char* line_) {
  char line0[MAX_LINE];
  memset(line0, 0, MAX_LINE);

  // preprocess line_ to erase '\t' chars.
  char* p = line_;
  char* lp = line0;
  while (*p != '\n') {
    if (*p != '\t') {
      *lp = *p;
      lp++;
    }
    p++;
    if (p - line_ >= MAX_LINE) {
      break;
    }
  }
  *lp = '\n';

  if (line0[0] == '\n') {
    // empty line.
    return 0;
  }

  // preprocess line_ to handle varied spacing before and after the '>' sign.
  char line1[MAX_LINE];
  memset(line1, 0, MAX_LINE);
  p = strchr(line0, '>');
  if (p != NULL) {
    // the offset between the start address of line_ and the '>' sign.
    const int off = p - line0;
    strncpy(line1, line0, off);
    line1[off] = ' ';
    line1[off + 1] = '>';
    line1[off + 2] = ' ';
    strcpy(line1 + off + 3, p + 1);
  } else {
    strcpy(line1, line0);
  }

  // handle varied spacing before and after the '&' signs.
  char l[MAX_LINE];
  memset(l, 0, MAX_LINE);
  strcpy(l, line1);
  char* ll = l;
  while (1) {
    p = strchr(ll, '&');
    if (p == NULL) {
      break;
    }
    int bad_spacing = 0;
    if (p > ll && *(p - 1) != ' ') {
      bad_spacing = 1;
    }
    if (p < ll + strlen(ll) - 2 && *(p + 1) != ' ') {
      bad_spacing = 1;
    }

    if (bad_spacing) {
      // the offset between the start address of line1 and the '>' sign.
      char line2[MAX_LINE];
      memset(line2, 0, MAX_LINE);
      const int off = p - ll;
      strncpy(line2, ll, off);
      line2[off] = ' ';
      line2[off + 1] = '&';
      line2[off + 2] = ' ';
      strcpy(line2 + off + 3, p + 1);
      strcpy(ll, line2);

      ll = ll + off + 2;
    } else {
      ll = p + 1;
    }

    if (p >= ll + strlen(ll)) {
      break;
    }
  }
  char line[MAX_LINE];
  memset(line, 0, MAX_LINE);
  strcpy(line, l);

  // tokenization.
  char* tokens[MAX_TOKENS];
  char* token = NULL;
  token = strtok(line, " ");
  int i = 0;
  for (; token != NULL; i++) {
    char* p = strchr(token, '\n');
    if (p != NULL) {
      token[strlen(token) - 1] = '\0';
      if (token[0] == '\0') {
        break;
      }
    }

    tokens[i] = token;
    token = strtok(NULL, " ");
  }

  const int num_tokens = i;

  if (num_tokens == 0) {
    // empty command line.
    return 0;
  }

  // sliding window to find all groups. Each group consists the cmd and its
  // args.
  int num_groups = 0;
  int begin = 0;
  int end = 0;
  while (end < num_tokens) {
    while (begin < num_tokens && !strcmp(tokens[begin], "&")) {
      begin++;
    }
    if (begin >= num_tokens) {
      break;
    }
    // tokens[begin] = cmd.

    end = begin;
    while (end < num_tokens && strcmp(tokens[end], "&")) {
      end++;
    }
    num_groups++;

    begin = end;
  }

  // no cmds.
  if (num_groups == 0) {
    return 0;
  }

  char*** groups = malloc(num_groups * sizeof(char**));
  int gi = 0;
  begin = 0;
  end = 0;
  while (end < num_tokens) {
    while (begin < num_tokens && !strcmp(tokens[begin], "&")) {
      begin++;
    }
    if (begin >= num_tokens) {
      break;
    }
    // tokens[begin] = cmd.

    end = begin;
    while (end < num_tokens && strcmp(tokens[end], "&")) {
      end++;
    }

    const int sz = end - begin;
    groups[gi] = malloc((sz + 1) * sizeof(char*));
    for (int i = 0; i < sz; i++) {
      groups[gi][i] = tokens[begin + i];
    }
    groups[gi][sz] = NULL;
    gi++;

    begin = end;
  }

  // execute each group in parallel.
  int* pids = malloc(num_groups * sizeof(int));
  memset(pids, 0, num_groups);
  int num_forks = 0;
  for (int i = 0; i < num_groups; i++) {
    if (execute_group(groups[i], pids, &num_forks)) {
      for (int j = 0; j < num_groups; j++) {
        free(groups[j]);
      }
      free(groups);
      free(pids);
      return 1;
    }
  }

  // parent waits to reap all children.
  for (int i = 0; i < num_forks; i++) {
    if (waitpid(pids[i], NULL, 0) == -1) {
      log_error();
      for (int j = 0; j < num_groups; j++) {
        free(groups[j]);
      }
      free(groups);
      free(pids);
      Exit(1);
    }
  }
  for (int j = 0; j < num_groups; j++) {
    free(groups[j]);
  }
  free(groups);
  free(pids);

  // if (fd != -1 && close(fd) == -1) {
  //   log_error();
  //   Exit(1);
  // }

  return 0;
}

void enter_interactive_mode() {
  for (;;) {
    // print prompt string.
    Write(STDOUT_FILENO, prompt_str, strlen(prompt_str));

    // read user input.
    if (getline(&line, &len, stdin) == -1) {
      // error or eof.
      break;
    }

    if (execute_line(line)) {
      break;
    }
  }
}

void enter_batch_mode(const char* batch_file) {
  FILE* f = fopen(batch_file, "r");
  if (f == NULL) {
    log_error();
    Exit(1);
  }
  int closed = 0;

  while (getline(&line, &len, f) != -1) {
    if (execute_line(line)) {
      Fclose(f);
      closed = 1;
      break;
    }
  }

  if (!closed) {
    Fclose(f);
  }
}

int main(int argc, char** argv) {
  if (argc > 2) {
    log_error();
    Exit(1);
  }

  init_path();

  if (argc == 1) {
    enter_interactive_mode();
  } else {
    enter_batch_mode(argv[1]);
  }

  Exit(0);
}