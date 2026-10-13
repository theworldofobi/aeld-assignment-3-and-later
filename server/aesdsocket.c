#include "aesdsocket.h"

static volatile sig_atomic_t caught_signal = 0;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

static void signal_handler(int signo)
{
  (void)signo;
  caught_signal = 1;
}

static int write_all(int fd, const char *data, size_t len)
{
  size_t written = 0;
  while (written < len) 
  {
    ssize_t n = write(fd, data + written, len - written);
    if (n < 0) 
    {
      syslog(LOG_ERR, "Write failed: %s", strerror(errno));
      return -1;
    }
    written += n;
  }
  return 0;
}

static int send_file(int client_fd, int fd)
{
  char buf[BUF_SIZE];
  ssize_t n;

  while ((n = read(fd, buf, sizeof(buf))) > 0) 
  {
    ssize_t sent = 0;
    while (sent < n) 
    {
      ssize_t s = send(client_fd, buf + sent, n - sent, 0);
      if (s < 0) 
      {
        syslog(LOG_ERR, "Send failed: %s", strerror(errno));
        return -1;
      }
      sent += s;
    }
  }

  if (n < 0)
  {
    syslog(LOG_ERR, "Read failed: %s", strerror(errno));
    return -1;
  }
  return 0;
}

// Parses "AESDCHAR_IOCSEEKTO:X,Y\n" where X and Y are unsigned decimal integers
static bool parse_seekto(const char *data, size_t len, struct aesd_seekto *seekto)
{
  const size_t prefix_len = strlen(SEEKTO_PREFIX);
  char args[32];
  char *end;

  if (len <= prefix_len || memcmp(data, SEEKTO_PREFIX, prefix_len) != 0) { return false; }

  size_t args_len = len - prefix_len - 1; // drop the trailing newline
  if (args_len == 0 || args_len >= sizeof(args)) { return false; }
  memcpy(args, data + prefix_len, args_len);
  args[args_len] = '\0';

  if (!isdigit((unsigned char)args[0])) { return false; }
  errno = 0;
  unsigned long cmd = strtoul(args, &end, 10);
  if (errno != 0 || cmd > UINT32_MAX || *end != ',') { return false; }

  const char *offset_str = end + 1;
  if (!isdigit((unsigned char)*offset_str)) { return false; }
  unsigned long offset = strtoul(offset_str, &end, 10);
  if (errno != 0 || offset > UINT32_MAX || *end != '\0') { return false; }

  seekto->write_cmd = cmd;
  seekto->write_cmd_offset = offset;
  return true;
}

// Writes the packet (or performs the seek it describes) and sends the file content
// back using the same file descriptor, so the file offset set by the ioctl is honored
static int process_packet(int client_fd, const char *data, size_t len)
{
  struct aesd_seekto seekto;
  int rc;

  int fd = open(DATA_FILE, DATA_OPEN_FLAGS, 0644);
  if (fd < 0) 
  {
    syslog(LOG_ERR, "Open failed: %s", strerror(errno));
    return -1;
  }

  if (parse_seekto(data, len, &seekto))
  {
    if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) != 0)
    {
      syslog(LOG_ERR, "Seek to %u,%u failed: %s", seekto.write_cmd, seekto.write_cmd_offset, strerror(errno));
    }
    rc = 0;
  }
  else
  {
    rc = write_all(fd, data, len);
    if (rc == 0 && lseek(fd, 0, SEEK_SET) < 0)
    {
      syslog(LOG_ERR, "Seek failed: %s", strerror(errno));
      rc = -1;
    }
  }

  if (rc == 0) { rc = send_file(client_fd, fd); }

  close(fd);
  return rc;
}

static void handle_client(int client_fd)
{
  char buf[BUF_SIZE];
  char *packet = NULL;
  size_t packet_len = 0;
  ssize_t n;

  while ((n = recv(client_fd, buf, sizeof(buf), 0)) > 0) 
  {
    char *tmp = realloc(packet, packet_len + n);
    if (tmp == NULL) 
    {
      syslog(LOG_ERR, "realloc failed, discarding packet");
      free(packet);
      packet = NULL;
      packet_len = 0;
      continue;
    }
    packet = tmp;
    memcpy(packet + packet_len, buf, n);
    packet_len += n;

    char *newline;
    while ((newline = memchr(packet, '\n', packet_len)) != NULL) 
    {
      size_t len = newline - packet + 1;

      pthread_mutex_lock(&file_mutex);
      int rc = process_packet(client_fd, packet, len);
      pthread_mutex_unlock(&file_mutex);

      if (rc != 0) 
      {
        syslog(LOG_ERR, "Error handling packet");
        free(packet);
        return;
      }

      memmove(packet, packet + len, packet_len - len);
      packet_len -= len;
    }
  }

  free(packet);
}

static void *connection_thread(void *arg)
{
  struct thread_node *node = arg;

  handle_client(node->client_fd);
  syslog(LOG_INFO, "Closed connection from %s", node->ip);

  node->done = true;
  return NULL;
}

#ifndef USE_AESD_CHAR_DEVICE
static int append_packet(const char *data, size_t len)
{
  int fd = open(DATA_FILE, DATA_OPEN_FLAGS, 0644);
  if (fd < 0) 
  {
    syslog(LOG_ERR, "Open for append failed: %s", strerror(errno));
    return -1;
  }

  int rc = write_all(fd, data, len);
  close(fd);
  return rc;
}

static void *timer_thread_func(void *arg)
{
  (void)arg;
  int seconds = 0;

  while (!caught_signal)
  {
    sleep(1);
    seconds++;
    if (seconds < 10) { continue; }
    seconds = 0;

    char ts[128];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    size_t len = strftime(ts, sizeof(ts), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm_now);
    if (len == 0) { continue; }

    pthread_mutex_lock(&file_mutex);
    append_packet(ts, len);
    pthread_mutex_unlock(&file_mutex);
  }

  return NULL;
}
#endif

static int start_thread(pthread_t *t, void *(*func)(void *), void *arg)
{
  sigset_t block_set, old_set;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGINT);
  sigaddset(&block_set, SIGTERM);

  pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
  int rc = pthread_create(t, NULL, func, arg);
  pthread_sigmask(SIG_SETMASK, &old_set, NULL);
  return rc;
}

static void join_threads(struct thread_list *head, bool wait_all)
{
  struct thread_node *node = SLIST_FIRST(head);

  while (node != NULL)
  {
    struct thread_node *next = SLIST_NEXT(node, entries);

    if (wait_all || node->done)
    {
      pthread_join(node->thread, NULL);
      close(node->client_fd);
      SLIST_REMOVE(head, node, thread_node, entries);
      free(node);
    }
    node = next;
  }
}

int main(int argc, char *argv[])
{
  bool run_as_daemon = false;
  if (argc == 2 && strcmp(argv[1], "-d") == 0) { run_as_daemon = true; }

  openlog("aesdsocket", LOG_PID, LOG_USER);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) 
  {
    syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
    return -1;
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) 
  {
    syslog(LOG_ERR, "Socket failed: %s", strerror(errno));
    return -1;
  }

  int opt = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) 
  {
    syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
    close(server_fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) 
  {
    syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
    close(server_fd);
    return -1;
  }

  if (run_as_daemon) 
  {
    pid_t pid = fork();
    if (pid < 0) {
      syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
      close(server_fd);
      return -1;
    }
    if (pid > 0) 
    { 
      int pid_fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (pid_fd >= 0)
      {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d\n", pid);
        
        int rc = write(pid_fd, pid_str, strlen(pid_str));
        if (rc < 0) { syslog(LOG_ERR, "Write PID file failed: %s", strerror(errno)); }
        
        close(pid_fd);
      }
      return 0; 
    }

    if (setsid() < 0 || chdir("/") != 0) 
    {
      syslog(LOG_ERR, "Daemon setup failed: %s", strerror(errno));
      close(server_fd);
      return -1;
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) { close(devnull); }
    }
  }

  if (listen(server_fd, BACKLOG) != 0) 
  {
    syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
    close(server_fd);
    return -1;
  }

#ifndef USE_AESD_CHAR_DEVICE
  pthread_t timer_thread;
  if (start_thread(&timer_thread, timer_thread_func, NULL) != 0)
  {
    syslog(LOG_ERR, "Timer thread create failed");
    close(server_fd);
    return -1;
  }
#endif

  struct thread_list head;
  SLIST_INIT(&head);

  while (!caught_signal) 
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) 
    {
      if (!caught_signal) { syslog(LOG_ERR, "accept failed: %s", strerror(errno)); }
      continue;
    }

    struct thread_node *node = malloc(sizeof(*node));
    if (node == NULL)
    {
      syslog(LOG_ERR, "malloc failed");
      close(client_fd);
      continue;
    }
    
    node->client_fd = client_fd;
    node->done = false;

    inet_ntop(AF_INET, &client_addr.sin_addr, node->ip, sizeof(node->ip));
    syslog(LOG_INFO, "Accepted connection from %s", node->ip);
    
    if (start_thread(&node->thread, connection_thread, node) != 0)
    {
      syslog(LOG_ERR, "pthread_create failed");
      close(client_fd);
      free(node);
      continue;
    }
    SLIST_INSERT_HEAD(&head, node, entries);

    join_threads(&head, false);
  }
  close(server_fd);
  syslog(LOG_INFO, "Caught signal, exiting");

  struct thread_node *node;
  SLIST_FOREACH(node, &head, entries) 
  { 
    shutdown(node->client_fd, SHUT_RDWR);
  }
  join_threads(&head, true);
#if !USE_AESD_CHAR_DEVICE
  pthread_join(timer_thread, NULL);
#endif
  pthread_mutex_destroy(&file_mutex);

#if !USE_AESD_CHAR_DEVICE
  remove(DATA_FILE);
#endif
  closelog();
  remove(PID_FILE);
  return 0;
}
