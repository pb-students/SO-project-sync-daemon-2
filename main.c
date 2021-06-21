#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/mman.h>

#define PERM_FILE 0644                  // uprawnienia tworzonych plików
#define PERM_DIR 0755                   // uprawnienia tworzonych katalogów
#define COPY_REGULAR_BUFSIZE 32*1024    // rozmiar bufora dla funkcji copyFileRegular (w bajtach)
#define COPY_MMAP_BUFSIZE 32*1024       // rozmiar bufora dla funkcji copyFileMmap (w bajtach)

char *PATH_IN, *PATH_OUT;           // ścieżki do synchronizowanych katalogów
int RECURSIVE = 0;                  // czy katalogi powinny być przeglądane rekursywnie
int SLEEP_TIME = 300;               // czas, jaki daemon ma przebywać w uśpieniu (w sekundach)
int MMAP_MIN_SIZE = 8*1024*1024;    // minimalny rozmiar pliku dla wykonania kopii z użyciem mmap (w bajtach)

// sprawdzenie typu obiektu, na jaki wskazuje dana ścieżka
int checkType(char* path)
{
    struct stat st;
    if (stat(path, &st) == 0)
    {
        if (st.st_mode & S_IFREG) return 0;         // plik
        else if (st.st_mode & S_IFDIR) return 1;    // katalog
        else return 2;                              // inny typ (np. block device, symlink)
    }
    else return -1; // stat error (np. plik nie istnieje)
}


// kopiowanie pliku przy użyciu read i write
int copyFileRegular(const char* pathin, const char* pathout, int bufsize)
{
    int srcf, destf;
    ssize_t nread;
    unsigned char* buffer;
    // alokacja pamięci dla bufora
    buffer = (unsigned char*) malloc(bufsize * sizeof(unsigned char));

    // próba otwarcia pliku źródłowego i docelowego
    if ((srcf = open(pathin, O_RDONLY)) == -1) return -1;
    if ((destf = open(pathout, O_WRONLY | O_CREAT | O_TRUNC, PERM_FILE)) == -1) { close(srcf); return -2; }

    // stopniowe wczytywanie do bufora zawartości pliku źródłowego i zapisywanie jej do pliku docelowego,
    // aż plik źródłowy nie będzie miał więcej danych
    while ((nread = read(srcf, buffer, bufsize)) > 0) write(destf, buffer, nread);
    
    free(buffer);               // zwolnienie pamięci bufora
    close(srcf); close(destf);  // zamknięcie plików
}

// kopiowanie pliku przy użyciu mmap i write
int copyFileMmap(const char *pathin, const char *pathout)
{
    char *srcmap;
    int remaining = 0;
    struct stat st; stat(pathin, &st);
    // otwarcie pliku źródłowego i docelowego
    int srcf = open(pathin, O_RDONLY), destf = open(pathout, O_WRONLY | O_CREAT | O_TRUNC, PERM_FILE);
    // zmapowanie pliku źródłowego w pamięci
    srcmap = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, srcf, 0);
    
    // alokacja pamięci dla bufora
    char *buffer = (unsigned char*) malloc(sizeof(char) * COPY_MMAP_BUFSIZE);
    int offset;
    for (offset = 0; offset < st.st_size + COPY_MMAP_BUFSIZE; offset += COPY_MMAP_BUFSIZE)
    {
        if (offset + COPY_MMAP_BUFSIZE > st.st_size)
        {
            remaining = st.st_size - offset;    
            //syslog(LOG_INFO, "offset + bufsize exceeds file size, offset: %d, remaining: %d\n", offset, remaining);
            break;
        }
        //syslog(LOG_INFO, "copying, offset: %d", offset);
        memcpy(buffer, srcmap + offset, COPY_MMAP_BUFSIZE); // skopiowanie fragmentu pliku źródłowego z pamięci do bufora
        write(destf, buffer, COPY_MMAP_BUFSIZE);            // zapis z bufora do pliku docelowego
    }
    // zwolnienie pamięci bufora
    free(buffer);

    // pozostały fragment
    if (remaining > 0)
    {
        buffer = malloc(sizeof(char) * remaining);  // alokacja pamięci dla bufora
        //syslog(LOG_INFO, "copying remaining, offset: %d, remaining: %d\n", offset, remaining);
        memcpy(buffer, srcmap + offset, remaining); // skopiowanie pozostałego fragmentu pliku źródłowego z pamięci do bufora
        write(destf, buffer, remaining);            // zapis z bufora do pliku docelowego
        free(buffer);                               // zwolnienie pamięci bufora
    }
    munmap(srcmap, st.st_size); // cofnięcie mapowania pliku źródłowego
    close(srcf); close(destf);  // zamknięcie plików
}

// ustalenie którą metodę kopiowania plików należy zastosować
int copyFileWrapper(const char *pathin, const char *pathout)
{
    struct stat st;
    stat(pathin, &st);
    if (st.st_size >= MMAP_MIN_SIZE)
    {
        copyFileMmap(pathin, pathout);
        syslog(LOG_INFO, "copied (mmap) %s to %s", pathin, pathout);
    }
    else
    {
        copyFileRegular(pathin, pathout, COPY_REGULAR_BUFSIZE);
        syslog(LOG_INFO, "copied (regular) %s to %s", pathin, pathout);
    }
}


// porównanie dwóch plików
int compareFiles(char *file1, char *file2)
{
    struct stat st1, st2;
    // wykonanie operacji stat na obu plikach
    errno = 0; stat(file1, &st1); if (errno == ENOENT) return 3;    // file1 nie istnieje
    errno = 0; stat(file2, &st2); if (errno == ENOENT) return 1;    // file2 nie istnieje
    if (st1.st_mtime > st2.st_mtime) return 2;                      // file1 ma nowszą datę modyfikacji niż file2
    return 0;                                                       // pliki są takie same (nie spełniają powyższych warunków)
}

int removeDir(char *path)
{
    DIR* dir;
    struct dirent* entry;
    int ret = 1;

    // przejście przez wszystkie obiekty w katalogu
    errno = 0; dir = opendir(path);
    while ((entry = readdir(dir)) != NULL)
    {
        // pominięcie "." (obecny katalog) i ".." (katalog nadrzędny)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        // połączenie ścieżki do obecnego katalogu i nazwy aktualnie analizowanego obiektu
        char temp1[1024]; snprintf(temp1, sizeof(temp1), "%s/%s", path, entry->d_name);

        // jeśli obiekt jest katalogiem, usunąć go i jego zawartość
        if (checkType(temp1) == 1) { syslog(LOG_INFO, "removing dir: %s", temp1); removeDir(temp1); }
        // jeśli obiekt jest plikiem, usunąć go
        if (checkType(temp1) == 0) { unlink(temp1); syslog(LOG_INFO, "removed file: %s", temp1); }
    }
    closedir(dir);
    // usuwanie katalogu
    rmdir(path);
    syslog(LOG_INFO, "removed dir: %s", path);
}

// porównanie katalogów
int compareDirs(char *pathin, char *pathout, int recursive)
{
    DIR* dir;
    struct dirent* entry;
    
    // jeśli katalog docelowy nie istnieje, utworzenie go
    if (checkType(pathout) == -1 && errno == ENOENT) { mkdir(pathout, PERM_DIR); syslog(LOG_INFO, "created dir: %s", pathout); }

    // sprawdzenie czy w src są obiekty, których nie ma w dest
    errno = 0; dir = opendir(pathin);
    while ((entry = readdir(dir)) != NULL)
    {
        // pominięcie "." (obecny katalog) i ".." (katalog nadrzędny)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        // utworzenie ścieżek do obu obiektów w obu katalogach
        char temp1[1024], temp2[1024];
        snprintf(temp1, sizeof(temp1), "%s/%s", pathin, entry->d_name);
        snprintf(temp2, sizeof(temp2), "%s/%s", pathout, entry->d_name);

        // jeśli obiekt jest katalogiem, to porównać jego zawartość
        if (recursive == 1 && checkType(temp1) == 1) compareDirs(temp1, temp2, 1);
        if (checkType(temp1) != 0) continue; // jeśli nie plik (i nie katalog), to przejść do następnego
        int temp = compareFiles(temp1, temp2);
        // jeśli pliku nie ma w dest lub ten w src ma datę nowszą niż w dest
        if (temp == 1 || temp == 2) copyFileWrapper(temp1, temp2);
    }
    closedir(dir);
    
    // sprawdzenie czy w dest są obiekty, których nie ma w src
    errno = 0; dir = opendir(pathout);
    while ((entry = readdir(dir)) != NULL)
    {
        // pominięcie "." (obecny katalog) i ".." (katalog nadrzędny)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char temp1[1024], temp2[1024];
        // utworzenie ścieżek do obu obiektów w obu katalogach
        snprintf(temp1, sizeof(temp1), "%s/%s", pathin, entry->d_name);
        snprintf(temp2, sizeof(temp2), "%s/%s", pathout, entry->d_name);

        if (checkType(temp2) == 2 || checkType(temp2) == -1) continue; // jeśli nie plik lub folder, to przejść do następnego

        // jeśli w src nie ma tego pliku/folderu
        if (compareFiles(temp1, temp2) == 3) 
        {
            // jeśli plik, to usunąć
            if (checkType(temp2) == 0) { unlink(temp2); syslog(LOG_INFO, "removed file: %s", temp2); }
            // jeśli folder, to opróżnić go i usunąć
            if (checkType(temp2) == 1) removeDir(temp2);
        }
    }
    closedir(dir);
}

// obsługa sygnału SIGUSR1
void signal_handler(int signum)
{
    syslog(LOG_INFO, "received SIGUSR1");
}



int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        printf("Usage: %s path_in path_out [-R] [-s sleep_time] [-m mmap_min_size].\n", argv[0]);
        printf("-R - sync directories recursively\n");
        printf("-s sleep_time - set sleep time for daemon (seconds)\n");
        printf("-m mmap_min_size - set minimal size for mmap-based copy (bytes)\n");
        exit(1);
    }
    if (checkType(argv[1]) != 1 || checkType(argv[2]) != 1)
    {
        printf("At least one of first two arguments isn't a directory.\n");
        exit(2);
    }

    // parsowanie argumentów
    PATH_IN = argv[1];
    PATH_OUT = argv[2];
    if (argc == 4 && strcmp(argv[3], "-R") == 0) RECURSIVE = 1;
    else if (argc == 5)
    {
        if (strcmp(argv[3], "-s") == 0) SLEEP_TIME = atoi(argv[4]);
        if (strcmp(argv[3], "-m") == 0) MMAP_MIN_SIZE = atoi(argv[4]);
    }
    else if (argc == 6)
    {
        if (strcmp(argv[3], "-R") == 0) RECURSIVE = 1;
        if (strcmp(argv[4], "-s") == 0) SLEEP_TIME = atoi(argv[5]);
        if (strcmp(argv[4], "-m") == 0) MMAP_MIN_SIZE = atoi(argv[5]);
    }
    else if (argc == 8)
    {
        if (strcmp(argv[3], "-R") == 0) RECURSIVE = 1;
        if (strcmp(argv[4], "-s") == 0) SLEEP_TIME = atoi(argv[5]);
        if (strcmp(argv[6], "-m") == 0) MMAP_MIN_SIZE = atoi(argv[7]);
    }
    else
    {
        printf("Incorrect count of arguments.\n");
        exit(3);
    }

    // daemon
    pid_t pid = fork();
    if (pid < 0) { printf("fork failed.\n"); exit(4); }
    if (pid > 0) { printf("daemon PID: %d\n", pid); exit(0); }
    umask(0);
    pid_t sid = setsid();
    if (sid < 0) { printf("setsid failed.\n"); exit(5); }
    // zamknięcie standardowego wejścia/wyjścia
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // obsługa sygnału SIGUSR1
    signal(SIGUSR1, signal_handler);
    // otwarcie syslogu
    openlog("SO_syncdaemon", LOG_PID, LOG_USER);

    while (1)
    {
        syslog(LOG_INFO, "woke up");
        compareDirs(PATH_IN, PATH_OUT, RECURSIVE);
        syslog(LOG_INFO, "sleeping for %d seconds", SLEEP_TIME);
        sleep(SLEEP_TIME);
    }
}
