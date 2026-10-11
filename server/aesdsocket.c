#include "aesdsocket.h"

static volatile sig_atomic_t caught_signal = 0;

static void signal_handler(int signo)
{
  (void)signo;
  caught_signal = 1;
}

static int append_packet(const char *data, size_t len)
{
  int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) 
  {
    syslog(LOG_ERR, "Open for append failed: %s", strerror(errno));
    return -1;
  }

  size_t written = 0;
  while (written < len) 
  {
    ssize_t n = write(fd, data + written, len - written);
    if (n < 0) 
    {
      syslog(LOG_ERR, "Write failed: %s", strerror(errno));
      close(fd);
      return -1;
    }
    written += n;
  }

  close(fd);
  return 0;
}

static int send_file(int client_fd)
{
  char buf[BUF_SIZE];
  ssize_t n;

  int fd = open(DATA_FILE, O_RDONLY);
  if (fd < 0) 
  {
    syslog(LOG_ERR, "Open for read failed: %s", strerror(errno));
    return -1;
  }

  while ((n = read(fd, buf, sizeof(buf))) > 0) 
  {
    ssize_t sent = 0;
    while (sent < n) 
    {
      ssize_t s = send(client_fd, buf + sent, n - sent, 0);
      if (s < 0) 
      {
        syslog(LOG_ERR, "Send failed: %s", strerror(errno));
        close(fd);
        return -1;
      }
      sent += s;
    }
  }

  close(fd);
  return 0;
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
      if (append_packet(packet, len) == 0) { send_file(client_fd);}
      memmove(packet, packet + len, packet_len - len);
      packet_len -= len;
    }
  }

  free(packet);
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
        write(pid_fd, pid_str, strlen(pid_str));
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

  while (!caught_signal) 
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    char ip[INET_ADDRSTRLEN];

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) 
    {
      if (!caught_signal) { syslog(LOG_ERR, "accept failed: %s", strerror(errno)); }
      continue;
    }

    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    syslog(LOG_INFO, "Accepted connection from %s", ip);

    handle_client(client_fd);

    close(client_fd);
    syslog(LOG_INFO, "Closed connection from %s", ip);
  }

  syslog(LOG_INFO, "Caught signal, exiting");
  close(server_fd);
  remove(DATA_FILE);
  closelog();
  remove(PID_FILE);
  return 0;
}
