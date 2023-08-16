/******************************************************************************
** Program:     Small Shell
** Author:      Blake Roberts
** ID:          934-254-047
** Date:        2023-05-21
** Description: smallsh implements a command line interface similar to 
**              well-known shells, such as bash. The program 
**
**                - Prints an interactive input prompt
**                - Parses command line input into semantic tokens
**                - Implements parameter expansion
**                - Interprets shell special parameters $$, $?, and $! and 
**                  generic parameters as ${parameter}
**                - Implements two shell built-in commands: exit and cd
**                - Execute non-built-in commands using the the appropriate 
**                  EXEC(3) function.
**                - Implements redirection operators ‘<’,  ‘>’ and '>>'
**                - Implements the ‘&’ operator to run commands in the 
**                  background
**                - Implement custom behavior for SIGINT and SIGTSTP signals
** 
******************************************************************************/
//INCLUDES/////////////////////////////////////////////////////////////////////

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

//MACROS///////////////////////////////////////////////////////////////////////

/* Debug functionality adapted from Tree Assignment - CS344 @ OSU */
#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

#ifndef MAX_WORDS
#define MAX_WORDS 1024
#endif

//FUNCTIONS////////////////////////////////////////////////////////////////////

char *words[MAX_WORDS];
size_t wordsplit(char const *line);
char * expand(char const *word);
void sigint_handler(int sig);
int n_digits_counter(int n);
FILE * open_read(char *read);
FILE * open_write(char *write);
FILE * open_append(char *append);
void * update_output_descriptors(FILE *output);
void * update_input_descriptors(FILE *input);

//GLOBALS//////////////////////////////////////////////////////////////////////

int last_status = 0;
int last_bg_pid = 0;

//MAIN/////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{

//I/O//////////////////////////////////////////////////////////////////////////

  FILE *og_input = stdin; // default input
  FILE *output = stderr;  // default output

  // Initialize I/O
  char *input_fn = "(stdin)";
  if (argc == 2) {
    input_fn = argv[1];
    og_input = fopen(input_fn, "re");
    if (!og_input) err(1, "%s", input_fn);
  } else if (argc > 2) {
    errx(1, "too many arguments");
  }

  FILE *input = og_input; // working input
  char *line = NULL;
  size_t n = 0;

  // Set input stream to close-on-exec
  if (input != stdin) {
    int input_fileno = fileno(input);
    fcntl(input_fileno, F_SETFD, FD_CLOEXEC);
  }

//SIGNALHANDLING///////////////////////////////////////////////////////////////

  // Initialize signal structs
  struct sigaction  sigint_act  = {0}, // set signal structs
                    sigint_old  = {0}, 
                    sigtstp_act = {0},
                    sigtstp_old = {0};

  if (input == stdin) { // only handle specially in interactive mode
    dprintf("Signals set for interactive mode.\n");
    
    // SIGSTP
    sigtstp_act.sa_handler = SIG_IGN;
    if (sigaction(SIGTSTP, &sigtstp_act, &sigtstp_old) == -1) {
      dprintf("Signal ignore failed on SIGTSTP\n");
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
    } // Always ignore SIGTSTP

    // SIGINT
    sigint_act.sa_handler = sigint_handler;
    sigfillset(&sigint_act.sa_mask);
    sigint_act.sa_flags = 0;
    if (sigaction(SIGINT, &sigint_act, &sigint_old) == -1) {
      dprintf("Signal ignore failed on SIGSTP\n");
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
    }    // Set to empty handler
  }

//INFINITELOOP/////////////////////////////////////////////////////////////////

  for (;;) {

//PROMPT///////////////////////////////////////////////////////////////////////

prompt:;

    // Manage bg processes
    int bg_status = 0;
    pid_t bg_pid = waitpid(0, &bg_status, WNOHANG | WUNTRACED);

    if (bg_pid != -1 && bg_pid != 0) {
      if (WIFEXITED(bg_status)) {
        fprintf(stderr, 
        "Child process %jd done. Exit status %d.\n", 
        (intmax_t) bg_pid, WEXITSTATUS(bg_status));
      }
      if (WIFSIGNALED(bg_status)) {
        fprintf(stderr, 
        "Child process %jd done. Signaled %d.\n", 
        (intmax_t) bg_pid, WTERMSIG(bg_status));
      }
      if (WIFSTOPPED(bg_status)) {
        fprintf(stderr, 
        "Child process %jd stopped. Continuing.\n", 
        (intmax_t) bg_pid);
        kill(bg_pid, SIGCONT);
      }
    }

    // Reset working vars
    clearerr(input);  // clear errors
    errno = 0;        // reset errno
    input = og_input; // reset input
    output = stderr;  // reset output

    // Print prompt
    if (input == stdin) {
      // interactive mode
      fprintf(stderr, "%s", expand("${PS1}"));
    }

//FLAGS////////////////////////////////////////////////////////////////////////

    bool bg       = false;
    char *read    = NULL;
    char *write   = NULL;
    char *append  = NULL;

//GETLINE//////////////////////////////////////////////////////////////////////
    
    // Read line of input
    ssize_t line_len = getline(&line, &n, input);
    dprintf("getline executed\n");
    if (line_len < 0) {
      dprintf("getline returned -1: error\n");
      if (feof(input)) {
        goto default_exit;
      } else if (ferror(input)) {
        dprintf("Loading prompt.\n");
        fprintf(output, "\n");
        goto prompt;
      }
    }

    // Start fully ignoring SIGINT
    if (input == stdin) { 
      sigint_act.sa_handler = SIG_IGN;
      sigaction(SIGINT, &sigint_act, NULL);
    } 
    
    
//SPLIT/EXPAND/////////////////////////////////////////////////////////////////
    
    // Wordsplit input
    dprintf("executing wordsplit...\n");
    size_t n_words = wordsplit(line);
    dprintf("wordsplit executed\n");

    // Expand input
    dprintf("Executing expansion\n");
    for (size_t i = 0; i < n_words; ++i) {
      dprintf("Word %zu: %s  -->  ", i, words[i]);
      char *exp_word = expand(words[i]);
      free(words[i]);
      words[i] = exp_word;
      dprintf("%s\n", words[i]);
    }

#ifdef DEBUG // Check Expanded string
  dprintf("Expanded input: ");
  for (int i = 0; i < n_words; i++) {
    dprintf("%s ", words[i]);
  }
  dprintf("\n");
#endif

//PARSING//////////////////////////////////////////////////////////////////////

    // Parse words into tokens and deal with redirection operators
    int i = 0;
    int n_tokens = 0;
    int op = 0;
    char *tokens[MAX_WORDS];

    for (i; i < n_words; i++) {
      if (i == n_words - 1 && strcmp(words[i], "&") == 0) {
        bg = true;
        op++;
        continue;
      }
      if (strcmp(words[i], ">") == 0) {
        // write
        write = words[i + 1];
        i++;
        op = op + 2;
        output = open_write(write);
        fclose(output);
        continue;
      }
      if (strcmp(words[i], "<") == 0) {
        // read
        read = words[i + 1];
        i++;
        op = op + 2;
        continue;
      }
      if (strcmp(words[i], ">>") == 0) {
        // append
        append = words[i + 1];
        i++;
        op = op + 2;
        continue;
      }
      tokens[i - op] = words[i];
      dprintf("Token %d: %s\n", i - op, tokens[i - op]);
      n_tokens++;
    }
    tokens[i - op] = NULL;
    dprintf("Token %d: %s\n", i - op, tokens[i - op]);
    n_tokens++;

#ifdef DEBUG //Check parsed string
  dprintf("Parsed input: ");
  for (int i = 0; i < n_tokens; i++) {
    if (tokens[i] != NULL) {
      dprintf("%s ", tokens[i]);
    } else {
      dprintf("(null)");
    } 
  }
  //print flags
  dprintf("\nPrinting flags/globals...\n");
  dprintf("bg: %d\n", bg);
  if (write != NULL) {
    dprintf("write: %s\n", write);
  } else {
    dprintf("write: (null)\n");
  }
  if (read != NULL) {
    dprintf("read: %s\n", read);
  } else {
    dprintf("read: (null)\n");
  }
  if (append != NULL) {
    dprintf("append: %s\n", append);
  } else {
    dprintf("append: (null)\n");
  } 
#endif

//EXECUTION////////////////////////////////////////////////////////////////////

    // No command given
    dprintf("executing commands\n");
    if (tokens[0] == NULL) {
      dprintf("No command given.\n");
      goto prompt;

//EXIT/////////////////////////////////////////////////////////////////////////

    // Built-in exit command
    } else if (strcmp(tokens[0], "exit") == 0) {
      if (n_tokens - 1 >= 3) {
        errno = E2BIG; // Too many arguments
        fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      } else if (n_tokens - 1 == 2) {
        if (isdigit(*tokens[1]))
        { // Check if argument is an integer
          dprintf("Exit code: %s\n", tokens[1]);
          exit(atoi(tokens[1]));  // Exit with argument
        } else {
          errno = EINVAL; // Argument is not an integer
          fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
          goto prompt;
        }
      } else {
default_exit:;
        dprintf("Default exit: %s", expand("$?"));
        exit(atoi(expand("$?")));
      } 

//CD///////////////////////////////////////////////////////////////////////////
     
    // Built-in change dir command

    } else if (strcmp(tokens[0], "cd") == 0) {

#ifdef DEBUG
  char *cwd = NULL;
#endif  

      // Do cd stuff
      if (n_tokens - 1 >= 3) {
        errno = E2BIG; // too many arguments
        fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      } else if (n_tokens - 1 == 2) {
        if (access(tokens[1], F_OK) != -1) {
          dprintf("Changing directory to: %s\n", words[1]);
          if (chdir(tokens[1]) != 0) { // change dir to arg
            errno = ENOENT; // chdir failed
            fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
          }

#ifdef DEBUG
  dprintf("New directory is %s\n", getcwd(cwd, 0));
  free(cwd);
#endif 

        } else {
          errno = ENOTDIR; // not a directory
          fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
        }
      } else {
        // change dir to home
        dprintf("Changing directory to: %s\n", expand("${HOME}"));
        if (chdir(expand("${HOME}")) != 0) {
          errno = ENOENT; //chdir failed
          fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
        }

#ifdef DEBUG
  dprintf("New directory is %s\n", getcwd(cwd, 0));
  free(cwd);
#endif

        goto prompt;
      }

//EXECVP///////////////////////////////////////////////////////////////////////

    // Execute non built-in commands as child processes

    } else {
      // Do execvp stuff
      pid_t fork_pid  = -13; // set to bogus numbers in case of bad luck
      pid_t child_pid = -13;
      int child_exit  = -13;

      fork_pid = fork();

      if (fork_pid == -1) {
        // fork failed
        fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
        goto prompt;
      } else if (fork_pid == 0) {
        // Child Process

        // Reset signals
        if (sigaction(SIGINT, &sigint_old, NULL) == -1 
        || sigaction(SIGTSTP, &sigtstp_old, NULL) == -1) {
          dprintf("Signal restoration failed");
          fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
          goto prompt;
        }

        // Set I/O and descriptors
        if (append != NULL) {
          // Do append stuff
          output = open_append(append);
          update_output_descriptors(output);
        }
        if (write != NULL) {
          // Do Write stuff
          output = open_write(write);
          update_output_descriptors(output);
        }
        if (read != NULL) {
          // Do read stuff
          input = open_read(read);
          update_input_descriptors(input);
        }

        // Execute command
        if (execvp(tokens[0], tokens) < 0) {
          dprintf("execvp failed.\n");
          fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
          exit(EXIT_FAILURE);
        } else {
          dprintf("Child #%d Process Executed.\n", (intmax_t) getpid());
          exit(EXIT_SUCCESS);
        }
      }
      // Parent Process
      if (bg == true) {
        // Don't wait on child process
        last_bg_pid = fork_pid;
        dprintf("Child #%d running in background.\n", fork_pid);
        goto prompt;
      } else {
        // Wait on child process
        dprintf("Parent #%d waiting for child #%d\n", 
        (intmax_t) getpid(), (intmax_t) fork_pid);
        child_pid = waitpid(fork_pid, &child_exit, WUNTRACED);

        // Manage signals
        if (WIFSIGNALED(child_exit)) {
          dprintf("Child signaled with %d. $? set to %d\n",
          WTERMSIG(child_exit), 128 + WTERMSIG(child_exit));
          last_status = 128 + WTERMSIG(child_exit);
        } else if (WIFSTOPPED(child_exit)) {
          fprintf(stderr, 
          "Child process %jd stopped. Continuing.\n", 
          (intmax_t) fork_pid);
          last_bg_pid = fork_pid;
          last_status = child_exit;
          kill(last_bg_pid, SIGCONT);
          goto prompt;
        } else {
          last_status = WEXITSTATUS(child_exit);
        }

        dprintf("child #%d terminated with exit status #%d\n", 
        (intmax_t) child_pid, WEXITSTATUS(last_status));
        goto prompt;
      }
    }
    goto prompt;
  } 
}

//FUNCTIONS////////////////////////////////////////////////////////////////////

char *words[MAX_WORDS] = {0};

/* Splits a string into words delimited by whitespace. Recognizes
 * comments as '#' at the beginning of a word, and backslash escapes.
 *
 * Returns number of words parsed, and updates the words[] array
 * with pointers to the words, each as an allocated string.
 */
size_t wordsplit(char const *line) {
  size_t wlen = 0;
  size_t wind = 0;

  char const *c = line;
  for (;*c && isspace(*c); ++c); /* discard leading space */

  for (; *c;) {
    if (wind == MAX_WORDS) break;
    /* read a word */
    if (*c == '#') break;
    for (;*c && !isspace(*c); ++c) {
      if (*c == '\\') ++c;
      void *tmp = realloc(words[wind], sizeof **words * (wlen + 2));
      if (!tmp) err(1, "realloc");
      words[wind] = tmp;
      words[wind][wlen++] = *c; 
      words[wind][wlen] = '\0';
    }
    ++wind;
    wlen = 0;
    for (;*c && isspace(*c); ++c);
  }
  return wind;
}


/* Find next instance of a parameter within a word. Sets
 * start and end pointers to the start and end of the parameter
 * token.
 */

char
param_scan(char const *word, char const **start, char const **end)
{
  static char const *prev;
  if (!word) word = prev;
  
  char ret = 0;
  *start = 0;
  *end = 0;
  for (char const *s = word; *s && !ret; ++s) {
    s = strchr(s, '$');
    if (!s) break;
    switch (s[1]) {
    case '$':
    case '!':
    case '?':
      ret = s[1];
      *start = s;
      *end = s + 2;
      break;
    case '{':;
      char *e = strchr(s + 2, '}');
      if (e) {
        ret = s[1];
        *start = s;
        *end = e + 1;
      }
      break;
    }
  }
  prev = *end;
  return ret;
}

/* Simple string-builder function. Builds up a base
 * string by appending supplied strings/character ranges
 * to it.
 */
char *
build_str(char const *start, char const *end)
{
  static size_t base_len = 0;
  static char *base = 0;

  if (!start) {
    /* Reset; new base string, return old one */
    char *ret = base;
    base = NULL;
    base_len = 0;
    return ret;
  }
  /* Append [start, end) to base string 
   * If end is NULL, append whole start string to base string.
   * Returns a newly allocated string that the caller must free.
   */
  size_t n = end ? end - start : strlen(start);
  size_t newsize = sizeof *base *(base_len + n + 1);
  void *tmp = realloc(base, newsize);
  if (!tmp) err(1, "realloc");
  base = tmp;
  memcpy(base + base_len, start, n);
  base_len += n;
  base[base_len] = '\0';

  return base;
}

/* Expands all instances of $! $$ $? and ${param} in a string 
 * Returns a newly allocated string that the caller must free
 */
char *
expand(char const *word)
{
  char const *pos = word;
  char const *start, *end;
  char c = param_scan(pos, &start, &end);
  build_str(NULL, NULL);
  build_str(pos, start);
  while (c) {
    if (c == '!') {
      int n = n_digits_counter(last_bg_pid);
      char bgpid_str[n];
      snprintf(bgpid_str, n, "%d", last_bg_pid);
      build_str(bgpid_str, NULL);
    } else if (c == '$') {
      int pid = getpid();
      int n = n_digits_counter(pid);
      char pid_str[n];
      snprintf(pid_str, n, "%d", pid);
      build_str(pid_str, NULL);
    } else if (c == '?') {
      int n = n_digits_counter(last_status);
      if (last_status == 0) {
        build_str("0", NULL);
      } else {
        char stat_str[n];
        snprintf(stat_str, n, "%d", last_status);
        build_str(stat_str, NULL);
      }
    }
    else if (c == '{') {
      int param_digits = end - start - 3;
      char param_arg[64] = {0};
      strncpy(param_arg, start + 2, param_digits);
      char *param_str = getenv(param_arg);
      if (param_str == NULL) {
        build_str("", NULL);
      } else {
        build_str(param_str, NULL);
      }
    }
    pos = end;
    c = param_scan(pos, &start, &end);
    build_str(pos, start);
  }
  return build_str(start, NULL);
}

void 
sigint_handler(int sig) {
  dprintf("\nsigint_handler called.\n");
}

int
n_digits_counter(int n) {
  int i = 1;
  while (n > 0) {
    n /= 10;
    i++;
  }
  return i;
}

FILE *
open_read(char * read) {
  FILE* input = fopen(read, "re");
  if (input) {
    dprintf("Input stream set to %s\n", read);
  } else {
    fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
  return input;
}

FILE *
open_write(char *write) {
  FILE* output;
  if (access(write, W_OK) == -1) {
  // File doesn't exist, so create write-only with perms 0777
    output = fopen(write, "w");
    if (output) {
      if (chmod(write, 511) != -1) {
        dprintf("File \"%s\" created with permission 0777\n", write);
      } else {
        dprintf("chmod failed.\n");
        fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
        exit(EXIT_FAILURE);        
      }
    } else {
      dprintf("Failed to create file %s\n", write);
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
    } else {
    output = fopen(write, "w");
    if (output) {
      dprintf("Output stream set to %s\n", write);
    } else {
      dprintf("Failed to open output stream.\n");
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
  }
  return output;
}

FILE *
open_append(char *append) {
  FILE* output;
  if (access(append, W_OK) == -1) {
    // File doesn't exist, so create write-only with perms 0777
    output = fopen(append, "a");
    if (output) {
      if (chmod(append, 511) != -1) {
        dprintf("File \"%s\" created with permission 0777\n", append);
      } else {
        dprintf("chmod failed.\n");
        fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
        exit(EXIT_FAILURE);        
      }
    } else {
      dprintf("Failed to create file %s\n", append);
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
  } else {
    output = fopen(append, "a");
    if (output) {
      dprintf("Output stream set to %s\n", append);
    } else {
      dprintf("Failed to open output stream.\n");
      fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
  }
  return output;
}

void * 
update_input_descriptors(FILE *input) {
  if (dup2(fileno(input), STDIN_FILENO) != -1) {
    dprintf("STDIN_FILENO updated to %d\n", fileno(input));
  } else {
    errno = EBADF;
    fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

void * 
update_output_descriptors(FILE *output) {
  if (dup2(fileno(output), STDOUT_FILENO) != -1) {
    dprintf("STDOUT_FILENO updated to %d\n", fileno(output));
  } else {
    errno = EBADF;
    fprintf(stderr, "Error No. %d: %s\n", errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

