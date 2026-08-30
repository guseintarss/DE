/*
 * de-workspace — мост между оболочкой DE и рабочими столами wayfire.
 *
 * Оболочка (QuickShell: TopBar.qml, BarMenu.qml) показывает точки рабочих
 * столов и пишет команды в FIFO $XDG_RUNTIME_DIR/de/ws-cmd, а состояние
 * читает из файла $XDG_RUNTIME_DIR/de/workspaces ("<текущий> <всего>").
 *
 * Этот демон:
 *   1. создаёт $XDG_RUNTIME_DIR/de/ и FIFO ws-cmd (если их ещё нет);
 *   2. слушает ws-cmd и по команде переключает рабочий стол композитора
 *      IPC-методом vswitch/set-workspace по сокету wayfire;
 *   3. поддерживает актуальным файл workspaces.
 *
 * Автозапускается wayfire (секция [autostart] в config/wayfire.ini), поэтому
 * наследует переменную WAYFIRE_SOCKET, которую композитор выставляет в своём
 * окружении при старте IPC-плагина.
 *
 * Сборка: meson (использует только libc + POSIX-сокеты).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ─── Утилиты пути/каталога ────────────────────────────────────────────── */

static char runtime_dir[PATH_MAX];
static char de_dir[PATH_MAX];
static char fifo_path[PATH_MAX];
static char state_path[PATH_MAX];

static void ensure_de_dir(const char *rt)
{
    snprintf(de_dir, sizeof de_dir, "%s/de", rt);
    mkdir(de_dir, 0755);

    snprintf(fifo_path, sizeof fifo_path, "%s/ws-cmd", de_dir);
    snprintf(state_path, sizeof state_path, "%s/workspaces", de_dir);

    /* Если FIFO нет — создаём. Старый не-FIFO файл удаляем. */
    struct stat st;
    if (lstat(fifo_path, &st) == 0)
    {
        if (!S_ISFIFO(st.st_mode))
        {
            unlink(fifo_path);
            mkfifo(fifo_path, 0666);
        }
    } else
    {
        mkfifo(fifo_path, 0666);
    }
}

/* ─── Запись состояния (текущий индекс, всего столов) ───────────────────── */

static void write_state(int current, int total)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%d %d\n", current, total);
    int fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        return;
    }

    ssize_t w = write(fd, buf, (size_t)(n > 0 ? n : 0));
    (void)w;
    close(fd);
}

/* ─── Связь с IPC-сокетом wayfire ───────────────────────────────────────── */
/* Протокол: заголовок 4 байта (длина, little-endian) + JSON-тело.
 * Запрос:  {"method": "<имя>", "data": { ... }}                        */

static int ipc_connect(const char *path)
{
    if (!path || !*path)
    {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static ssize_t write_exact(int fd, const void *data, size_t len)
{
    const char *p = data;
    size_t done   = 0;
    while (done < len)
    {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        done += (size_t)w;
    }

    return (ssize_t)done;
}

static int ipc_request(int fd, const char *method, const char *json_data)
{
    char body[2048];
    int len = snprintf(body, sizeof body, "{\"method\":\"%s\",\"data\":%s}",
        method, json_data);
    if (len <= 0 || len >= (int)sizeof body)
    {
        return -1;
    }

    uint32_t hdr = (uint32_t)len;
    if (write_exact(fd, &hdr, 4) < 0)
    {
        return -1;
    }

    return (write_exact(fd, body, (size_t)len) < 0) ? -1 : 0;
}

/* Читает один ответ (заголовок + тело). Полезно для синхронизации и для
 * получения данных get-focused-output. Возвращает 0 при успехе. */
static int ipc_read_response(int fd, char *out, size_t out_sz)
{
    uint32_t hdr;
    size_t got = 0;
    while (got < 4)
    {
        ssize_t r = read(fd, (char *)&hdr + got, 4 - got);
        if (r <= 0)
        {
            return -1;
        }

        got += (size_t)r;
    }

    if (hdr >= out_sz)
    {
        hdr = (uint32_t)out_sz - 1;
    }

    got = 0;
    while (got < hdr)
    {
        ssize_t r = read(fd, out + got, hdr - got);
        if (r <= 0)
        {
            return -1;
        }

        got += (size_t)r;
    }

    out[got] = '\0';
    return 0;
}

/* Ищет int по ключу "key":N в JSON-ответе (предсказуемый вывод wayfire). */
static int json_find_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out)
    {
        return -1;
    }

    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p)
    {
        return -1;
    }

    p += strlen(needle);
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }

    long v = strtol(p, NULL, 10);
    *out   = (int)v;
    return 0;
}

static int parse_focused_state(const char *resp,
    int *out_id, int *cur_x, int *cur_y, int *grid_w, int *grid_h)
{
    /* get-focused-output возвращает {"result":"ok","info":{...}}
     * нужные поля: info.id, info.workspace.x, info.workspace.grid_width ... */
    /*
     * get-focused-output возвращает {"result":"ok","info":{...}}.
     * Поля:
     *   "info": {"id":0,"name":"...","geometry":{...},
     *            "workarea":{...},"wset-index":0,
     *            "workspace":{"x":0,"y":0,"grid_width":2,"grid_height":2}, ...}
     * geometry/workarea не содержат ключа "id", поэтому первый "id": — это
     * id выхода. Поля рабочей области ищем внутри объекта "workspace".
     */
    const char *info = strstr(resp, "\"info\":");
    if (!info)
    {
        return -1;
    }

    if (out_id)
    {
        json_find_int(info, "id", out_id);
    }

    const char *wsobj = strstr(info, "\"workspace\":");
    if (!wsobj)
    {
        return -1;
    }

    if (cur_x)
    {
        json_find_int(wsobj, "x", cur_x);
    }

    if (cur_y)
    {
        json_find_int(wsobj, "y", cur_y);
    }

    if (grid_w)
    {
        json_find_int(wsobj, "grid_width", grid_w);
    }

    if (grid_h)
    {
        json_find_int(wsobj, "grid_height", grid_h);
    }

    return 0;
}

/* ─── Обработка команд переключения стола ────────────────────────────────── */

static int get_focused(int fd, int *out_id, int *cur_x, int *cur_y,
    int *grid_w, int *grid_h)
{
    if (ipc_request(fd, "window-rules/get-focused-output", "{}") < 0)
    {
        return -1;
    }

    char resp[4096];
    if (ipc_read_response(fd, resp, sizeof resp) < 0)
    {
        return -1;
    }

    return parse_focused_state(resp, out_id, cur_x, cur_y, grid_w, grid_h);
}

static int linear_to_grid(int n, int w, int h, int *x, int *y)
{
    /* n — 1-based линейный номер стола в сетке w×h (в строку). */
    int total = w * h;
    if (n < 1 || total < 1)
    {
        return -1;
    }

    int clamped = n > total ? total : n;
    *x = (clamped - 1) % w;
    *y = (clamped - 1) / w;
    return 0;
}

/* Переключает рабочий стол. Возвращает новый линейный индекс (1-based). */
static int switch_workspace(int fd, const char *cmd, int current, int total)
{
    int n = 0;
    if (strcmp(cmd, "next") == 0)
    {
        n = (current >= total) ? 1 : current + 1;
    } else if (strcmp(cmd, "prev") == 0)
    {
        n = (current <= 1) ? total : current - 1;
    } else
    {
        n = atoi(cmd);
    }

    if (n < 1)
    {
        return current;
    }

    int out_id = 0, ox = 0, oy = 0;
    int normal = 1; /* если не смогли получить фокус — рабочий стол 0 */

    int gx = 0, gy = 0, x = 0, y = 0;
    int grid_w = 2, grid_h = 2;

    if (get_focused(fd, &out_id, &ox, &oy, &grid_w, &grid_h) < 0)
    {
        return current;
    }

    if (linear_to_grid(n, grid_w, grid_h, &x, &y) < 0)
    {
        return current;
    }

    /* корректно синхронизируем total по сетке */
    int real_total = grid_w * grid_h;
    (void)normal;
    (void)gx;
    (void)gy;

    char data[128];
    snprintf(data, sizeof data,
        "{\"output-id\":%d,\"x\":%d,\"y\":%d}", out_id, x, y);
    if (ipc_request(fd, "vswitch/set-workspace", data) < 0)
    {
        return current;
    }

    /* ответ не критичен, но прочитаем, чтобы держать сокет в синхроне */
    char resp[1024];
    ipc_read_response(fd, resp, sizeof resp);
    return n;
}

/* ─── Главный цикл ───────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt)
    {
        rt = "/tmp";
    }

    snprintf(runtime_dir, sizeof runtime_dir, "%s", rt);
    ensure_de_dir(rt);

    /* Предварительно: хотя бы пустой файл состояния, чтобы оболочка сразу
     * знала число столов (2×2 = 4). */
    write_state(1, 4);

    int state_fd = -1;
    if ((state_fd = open(fifo_path, O_RDONLY | O_NONBLOCK)) < 0)
    {
        return 1;
    }

    /* Открыта для чтения; снимем NONBLOCK, чтобы блокироваться на readLine,
     * но держа дескриптор открытым (иначе '>' у писателя может заблокироваться).
     * NONBLOCK сохраним: отдельные короткие строки легко вычитать циклом. */

    int current = 1;
    int total   = 4;

    char line[64];
    size_t pos = 0;

    int ipc = -1;
    const char *sock = getenv("WAYFIRE_SOCKET");

    for (;;)
    {
        /* Держим соединение с композитором (с retry, пока сокет не поднимется). */
        if (ipc < 0)
        {
            ipc = ipc_connect(sock);
            if (ipc >= 0)
            {
                /* Уточняем реальное число столов и текущий. */
                int id = 0, cx = 0, cy = 0, gw = 2, gh = 2;
                if (get_focused(ipc, &id, &cx, &cy, &gw, &gh) == 0)
                {
                    int t = gw * gh;
                    if (t > 0)
                    {
                        total = t;
                    }

                    current = cx + cy * gw + 1;
                    if (current < 1 || current > total)
                    {
                        current = 1;
                    }

                    write_state(current, total);
                }
            } else
            {
                write_state(current, total);
            }
        }

        /* Читаем команду из FIFO. */
        ssize_t r = read(state_fd, line + pos, sizeof line - 1 - pos);
        if (r > 0)
        {
            pos += (size_t)r;
            size_t nl;
            while ((nl = memchr(line, '\n', pos)) != NULL)
            {
                size_t clen = (size_t)(nl - line);
                line[clen] = '\0';

                if (ipc >= 0)
                {
                    int newcur = switch_workspace(ipc, line, current, total);
                    int t = total;
                    if (get_focused(ipc, NULL, NULL, NULL, NULL, NULL) == 0)
                    {
                        /* total не изменится; оставляем */
                    }
                    current = newcur;
                    write_state(current, t);
                }

                memmove(line, nl + 1, pos - clen - 1);
                pos -= clen + 1;
            }
        } else if (r < 0 && errno == EAGAIN)
        {
            /* нет данных — немного подождём */
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
        } else if (r == 0)
        {
            /* писатель закрыл FIFO: переоткроем */
            close(state_fd);
            state_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
            if (state_fd < 0)
            {
                state_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
                if (state_fd < 0)
                {
                    return 1;
                }
            }
        }
    }

    return 0;
}
