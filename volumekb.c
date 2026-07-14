#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include <getopt.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

// Настройки по умолчанию
#define BAR_WIDTH 250
#define BAR_HEIGHT 45
int vol_step = 2;
int timeout_ms = 2000;
int run_in_foreground = 0;

Display *dpy;
Window win;
int target_vol = 0;
float displayed_vol = 0;
float current_opacity = 0.0f;
int show_timer = 0;
int is_muted = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int epfd; 

// Блокировка повторного запуска
void check_single_instance() {
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "/tmp/volumekb_%d.lock", getuid());
    
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) exit(1);
    
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        printf("volumekb is already running.\n");
        exit(0);
    }
}

// Тихая установка автозапуска
void ensure_autostart() {
    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%s/.config/autostart", getenv("HOME"));
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", config_dir);
    system(cmd);
    
    char desktop_path[1024];
    snprintf(desktop_path, sizeof(desktop_path), "%s/volumekb.desktop", config_dir);
    
    if (access(desktop_path, F_OK) != -1) return;
    
    char exe_path[512];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    if (len != -1) {
        exe_path[len] = '\0';
        FILE *f = fopen(desktop_path, "w");
        if (f) {
            fprintf(f, "[Desktop Entry]\nType=Application\nName=VolumeKB\n"
                       "Comment=Hardware Volume OSD Daemon\nExec=%s\n"
                       "Terminal=false\nNoDisplay=true\n", exe_path);
            fclose(f);
        }
    }
}

// Потокобезопасное обновление данных о громкости
void update_volume(int trigger_show) {
    FILE *f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@", "r");
    if (f) {
        char buf[128] = {0};
        if (fgets(buf, sizeof(buf), f) != NULL) {
            float v = -1.0f;
            int parsed = sscanf(buf, "Volume: %f", &v);
            int muted = (strstr(buf, "[MUTED]") != NULL);
            
            // Синхронизируем переменные для графического потока
            pthread_mutex_lock(&lock);
            if (parsed == 1 && v >= 0.0f) {
                target_vol = (int)(v * 100.0f);
            }
            is_muted = muted;
            if (trigger_show) {
                show_timer = timeout_ms;
            }
            pthread_mutex_unlock(&lock);
        }
        pclose(f);
    }
}

void set_volume(int delta) {
    if (delta == 0) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%%s", 
             abs(delta), delta > 0 ? "+" : "-");
    system(cmd);
    update_volume(1);
}

void toggle_mute() {
    system("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    update_volume(1);
}

void add_device_to_epoll(const char *path) {
    if (strstr(path, "event") == NULL) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
}

// Поток отрисовки X11
void* osd_thread(void* arg) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return NULL;
    
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes attrs;
    attrs.override_redirect = True; 
    attrs.background_pixel = BlackPixel(dpy, screen);
    
    int s_width = DisplayWidth(dpy, screen);
    int s_height = DisplayHeight(dpy, screen);
    
    win = XCreateWindow(dpy, root, 
                        (s_width - BAR_WIDTH) / 2, s_height - 150, 
                        BAR_WIDTH, BAR_HEIGHT, 0, 
                        CopyFromParent, InputOutput, CopyFromParent, 
                        CWOverrideRedirect | CWBackPixel, &attrs);
                        
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);
    
    XColor bg_col, bar_bg_col, fg_col, mute_col;
    Colormap cmap = DefaultColormap(dpy, screen);
    XAllocNamedColor(dpy, cmap, "#1a1a1a", &bg_col, &bg_col);
    XAllocNamedColor(dpy, cmap, "#404040", &bar_bg_col, &bar_bg_col);
    XAllocNamedColor(dpy, cmap, "#ffffff", &fg_col, &fg_col);
    XAllocNamedColor(dpy, cmap, "#ff4444", &mute_col, &mute_col);
    
    Atom opacity_atom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    int visible = 0;
    
    while (1) {
        pthread_mutex_lock(&lock);
        if (show_timer > 0) show_timer -= 16; 
        int target = target_vol;
        int muted = is_muted;
        float target_op = (show_timer > 0) ? 1.0f : 0.0f;
        pthread_mutex_unlock(&lock);
        
        current_opacity += (target_op - current_opacity) * 0.15f;
        displayed_vol += (target - displayed_vol) * 0.25f;
        
        if (current_opacity > 0.01f) {
            if (!visible) {
                XMapRaised(dpy, win);
                visible = 1;
            }
            unsigned long opac_val = (unsigned long)(current_opacity * 0xFFFFFFFF);
            XChangeProperty(dpy, win, opacity_atom, XA_CARDINAL, 32, PropModeReplace, 
                            (unsigned char *)&opac_val, 1);
            
            XSetForeground(dpy, gc, bg_col.pixel);
            XFillRectangle(dpy, win, gc, 0, 0, BAR_WIDTH, BAR_HEIGHT);
            
            unsigned long current_fg = muted ? mute_col.pixel : fg_col.pixel;
            char text[32];
            if (muted) snprintf(text, sizeof(text), "Volume: MUTED");
            else snprintf(text, sizeof(text), "Volume: %d%%", target);
            
            XSetForeground(dpy, gc, current_fg);
            XDrawString(dpy, win, gc, 15, 18, text, strlen(text));
            
            XSetForeground(dpy, gc, bar_bg_col.pixel);
            XFillRectangle(dpy, win, gc, 15, 25, BAR_WIDTH - 30, 8);
            
            XSetForeground(dpy, gc, current_fg);
            int fill_width = ((BAR_WIDTH - 30) * (int)displayed_vol) / 100;
            XFillRectangle(dpy, win, gc, 15, 25, fill_width, 8);
            XFlush(dpy);
        } else {
            if (visible) {
                XUnmapWindow(dpy, win);
                visible = 0;
            }
        }
        usleep(16000);
    }
    return NULL;
}

// Парсинг флагов запуска
void parse_args(int argc, char *argv[]) {
    struct option long_options[] = {
        {"step", required_argument, 0, 's'},
        {"timeout", required_argument, 0, 't'},
        {"foreground", no_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:t:f", long_options, NULL)) != -1) {
        if (opt == 's') vol_step = atoi(optarg);
        if (opt == 't') timeout_ms = atoi(optarg);
        if (opt == 'f') run_in_foreground = 1;
    }
}

int main(int argc, char *argv[]) {
    parse_args(argc, argv);
    check_single_instance();
    ensure_autostart();
    
    if (!run_in_foreground) {
        if (daemon(0, 0) < 0) exit(1);
    }

    // Первичное обновление звука без вызова OSD
    update_volume(0);
    displayed_vol = target_vol;
    
    pthread_t tid;
    pthread_create(&tid, NULL, osd_thread, NULL);
    
    epfd = epoll_create1(0);
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    
    if (inotify_fd >= 0) {
        inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = inotify_fd };
        epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev);
    }
    
    DIR *dir = opendir("/dev/input");
    struct dirent *ent;
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            char path[256];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            add_device_to_epoll(path);
        }
        closedir(dir);
    }
    
    struct epoll_event events[20];
    while (1) {
        int n = epoll_wait(epfd, events, 20, -1);
        for (int i = 0; i < n; i++) {
            // Если подключили новую USB клавиатуру
            if (events[i].data.fd == inotify_fd) {
                char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                while ((len = read(inotify_fd, buf, sizeof(buf))) > 0) {
                    const struct inotify_event *event;
                    for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
                        event = (const struct inotify_event *) ptr;
                        if (event->mask & IN_CREATE) {
                            char path[512];
                            snprintf(path, sizeof(path), "/dev/input/%s", event->name);
                            usleep(50000); // Ждем пока ядро выдаст права
                            add_device_to_epoll(path);
                        }
                    }
                }
            } else {
                // БАТЧИНГ: Читаем все скопившиеся ивенты разом
                int vol_delta = 0;
                int mute_toggled = 0;
                struct input_event ev;
                
                while (read(events[i].data.fd, &ev, sizeof(ev)) > 0) {
                    if (ev.type == EV_KEY && ev.value == 1) { 
                        if (ev.code == KEY_VOLUMEUP) vol_delta += vol_step;
                        else if (ev.code == KEY_VOLUMEDOWN) vol_delta -= vol_step;
                        else if (ev.code == KEY_MUTE) mute_toggled = 1;
                    }
                }
                
                // Исполняем системные команды ТОЛЬКО один раз на весь пакет
                if (mute_toggled) {
                    toggle_mute();
                }
                if (vol_delta != 0) {
                    set_volume(vol_delta);
                }
            }
        }
    }
    return 0;
}
