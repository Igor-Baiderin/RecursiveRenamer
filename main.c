#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <windowsx.h>

#define IDC_FOLDER     101
#define IDC_BROWSE     102
#define IDC_FIND       103
#define IDC_REPLACE    104
#define IDC_FILES      105
#define IDC_DIRS       106
#define IDC_RECURSIVE  107
#define IDC_PREVIEW    108
#define IDC_EXECUTE    109
#define IDC_LOG        110
#define IDC_STATUS     111

static HWND hFolder;
static HWND hFind;
static HWND hReplace;
static HWND hFiles;
static HWND hDirs;
static HWND hRecursive;
static HWND hLog;
static HWND hStatus;

static HFONT hFont;

static unsigned long g_scanned = 0;
static unsigned long g_matched = 0;
static unsigned long g_renamed = 0;
static unsigned long g_errors = 0;


/* ---------------------------------------------------------
   Журнал
   --------------------------------------------------------- */

static void log_append(const wchar_t *text)
{
    int len = GetWindowTextLengthW(hLog);

    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}


static void status_update(void)
{
    wchar_t buffer[256];

    swprintf(
        buffer,
        256,
        L"Просмотрено: %lu   Найдено: %lu   "
        L"Переименовано: %lu   Ошибок: %lu",
        g_scanned,
        g_matched,
        g_renamed,
        g_errors
    );

    SetWindowTextW(hStatus, buffer);
}


/* ---------------------------------------------------------
   Работа со строками
   --------------------------------------------------------- */

static wchar_t *duplicate_string(const wchar_t *src)
{
    size_t size = wcslen(src) + 1;

    wchar_t *result =
        (wchar_t *)malloc(size * sizeof(wchar_t));

    if (result != NULL)
        wcscpy(result, src);

    return result;
}


/*
    Заменяет ВСЕ вхождения find на replace.

    Возвращает:
        NULL - если find в строке не найден;
        новую строку - если замена возможна.

    Память результата нужно освободить через free().
*/
static wchar_t *replace_all(
    const wchar_t *src,
    const wchar_t *find,
    const wchar_t *replace
)
{
    size_t find_len = wcslen(find);
    size_t replace_len = wcslen(replace);
    size_t count = 0;

    const wchar_t *p;
    const wchar_t *q;

    if (find_len == 0)
        return duplicate_string(src);

    /* Считаем количество совпадений */

    p = src;

    while ((p = wcsstr(p, find)) != NULL)
    {
        count++;
        p += find_len;
    }

    if (count == 0)
        return NULL;

    size_t src_len = wcslen(src);

    size_t new_len;

    if (replace_len >= find_len)
    {
        new_len =
            src_len +
            count * (replace_len - find_len);
    }
    else
    {
        new_len =
            src_len -
            count * (find_len - replace_len);
    }

    wchar_t *result =
        (wchar_t *)malloc((new_len + 1) * sizeof(wchar_t));

    if (result == NULL)
        return NULL;

    wchar_t *dst = result;

    p = src;

    while ((q = wcsstr(p, find)) != NULL)
    {
        size_t part_len = (size_t)(q - p);

        wmemcpy(dst, p, part_len);
        dst += part_len;

        if (replace_len > 0)
        {
            wmemcpy(dst, replace, replace_len);
            dst += replace_len;
        }

        p = q + find_len;
    }

    wcscpy(dst, p);

    return result;
}


/* ---------------------------------------------------------
   Формирование пути
   --------------------------------------------------------- */

static int path_join(
    wchar_t *output,
    size_t capacity,
    const wchar_t *directory,
    const wchar_t *name
)
{
    size_t len = wcslen(directory);

    int need_slash =
        len > 0 &&
        directory[len - 1] != L'\\' &&
        directory[len - 1] != L'/';

    return swprintf(
        output,
        capacity,
        need_slash
            ? L"%ls\\%ls"
            : L"%ls%ls",
        directory,
        name
    ) > 0;
}


/* ---------------------------------------------------------
   Вывод найденной замены
   --------------------------------------------------------- */

static void report_change(
    const wchar_t *old_path,
    const wchar_t *new_path,
    int execute
)
{
    size_t size =
        wcslen(old_path) +
        wcslen(new_path) +
        64;

    wchar_t *line =
        (wchar_t *)malloc(size * sizeof(wchar_t));

    if (line == NULL)
        return;

    if (execute)
    {
        swprintf(
            line,
            size,
            L"OK: %ls  ->  %ls",
            old_path,
            new_path
        );
    }
    else
    {
        swprintf(
            line,
            size,
            L"%ls  ->  %ls",
            old_path,
            new_path
        );
    }

    log_append(line);

    free(line);
}


/* ---------------------------------------------------------
   Переименование
   --------------------------------------------------------- */

static int rename_item(
    const wchar_t *old_path,
    const wchar_t *new_path,
    int execute
)
{
    g_matched++;

    /*
        Режим проверки:
        ничего не изменяем.
    */

    if (!execute)
    {
        report_change(old_path, new_path, 0);
        return 1;
    }


    /*
        Проверяем, не существует ли уже объект
        с новым именем.
    */

    DWORD attributes =
        GetFileAttributesW(new_path);

    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        size_t size =
            wcslen(new_path) + 80;

        wchar_t *line =
            (wchar_t *)malloc(
                size * sizeof(wchar_t)
            );

        if (line != NULL)
        {
            swprintf(
                line,
                size,
                L"ПРОПУЩЕНО: уже существует: %ls",
                new_path
            );

            log_append(line);

            free(line);
        }

        g_errors++;

        return 0;
    }


    /*
        Windows MoveFile используется также
        для обычного переименования.
    */

    if (MoveFileW(old_path, new_path))
    {
        g_renamed++;

        report_change(
            old_path,
            new_path,
            1
        );

        return 1;
    }
    else
    {
        DWORD error = GetLastError();

        wchar_t line[1024];

        swprintf(
            line,
            1024,
            L"ОШИБКА %lu: %ls",
            error,
            old_path
        );

        log_append(line);

        g_errors++;

        return 0;
    }
}


/* ---------------------------------------------------------
   Рекурсивный обход каталога
   --------------------------------------------------------- */

static void walk_directory(
    const wchar_t *directory,
    const wchar_t *find,
    const wchar_t *replace,
    int execute,
    int process_files,
    int process_directories,
    int recursive
)
{
    wchar_t mask[32768];

    if (!path_join(
            mask,
            32768,
            directory,
            L"*"
        ))
    {
        return;
    }


    WIN32_FIND_DATAW find_data;

    HANDLE handle =
        FindFirstFileW(
            mask,
            &find_data
        );

    if (handle == INVALID_HANDLE_VALUE)
        return;


    do
    {
        /*
            "." и ".." пропускаем.
        */

        if (
            wcscmp(find_data.cFileName, L".") == 0 ||
            wcscmp(find_data.cFileName, L"..") == 0
        )
        {
            continue;
        }


        g_scanned++;


        wchar_t old_path[32768];

        if (!path_join(
                old_path,
                32768,
                directory,
                find_data.cFileName
            ))
        {
            continue;
        }


        int is_directory =
            (find_data.dwFileAttributes &
             FILE_ATTRIBUTE_DIRECTORY) != 0;

        int is_reparse_point =
            (find_data.dwFileAttributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0;


        /*
            КАТАЛОГ
        */

        if (is_directory)
        {
            /*
                ВАЖНО:

                Сначала обрабатываем содержимое
                каталога.

                Только после этого переименовываем
                сам каталог.

                Иначе путь изменится прямо во время
                рекурсивного обхода.
            */

            if (recursive && !is_reparse_point)
            {
                walk_directory(
                    old_path,
                    find,
                    replace,
                    execute,
                    process_files,
                    process_directories,
                    recursive
                );
            }


            if (process_directories)
            {
                wchar_t *new_name =
                    replace_all(
                        find_data.cFileName,
                        find,
                        replace
                    );


                if (new_name != NULL)
                {
                    /*
                        Не допускаем пустое имя.
                    */

                    if (*new_name != L'\0')
                    {
                        wchar_t new_path[32768];

                        path_join(
                            new_path,
                            32768,
                            directory,
                            new_name
                        );

                        rename_item(
                            old_path,
                            new_path,
                            execute
                        );
                    }
                    else
                    {
                        log_append(
                            L"ПРОПУЩЕНО: "
                            L"имя папки стало бы пустым."
                        );

                        g_errors++;
                    }

                    free(new_name);
                }
            }
        }

        /*
            ФАЙЛ
        */

        else if (process_files)
        {
            wchar_t *new_name =
                replace_all(
                    find_data.cFileName,
                    find,
                    replace
                );


            if (new_name != NULL)
            {
                if (*new_name != L'\0')
                {
                    wchar_t new_path[32768];

                    path_join(
                        new_path,
                        32768,
                        directory,
                        new_name
                    );

                    rename_item(
                        old_path,
                        new_path,
                        execute
                    );
                }
                else
                {
                    log_append(
                        L"ПРОПУЩЕНО: "
                        L"имя файла стало бы пустым."
                    );

                    g_errors++;
                }

                free(new_name);
            }
        }


        if ((g_scanned % 100) == 0)
        {
            status_update();
        }

    }
    while (
        FindNextFileW(
            handle,
            &find_data
        )
    );


    FindClose(handle);
}


/* ---------------------------------------------------------
   Запуск проверки / переименования
   --------------------------------------------------------- */

static void run_job(
    HWND hwnd,
    int execute
)
{
    wchar_t folder[32768];
    wchar_t find[1024];
    wchar_t replace[1024];


    GetWindowTextW(
        hFolder,
        folder,
        32768
    );

    GetWindowTextW(
        hFind,
        find,
        1024
    );

    GetWindowTextW(
        hReplace,
        replace,
        1024
    );


    /*
        Проверка параметров.
    */

    if (*folder == L'\0')
    {
        MessageBoxW(
            hwnd,
            L"Выберите папку.",
            L"Ошибка",
            MB_OK | MB_ICONWARNING
        );

        return;
    }


    if (*find == L'\0')
    {
        MessageBoxW(
            hwnd,
            L"Введите фрагмент, который "
            L"нужно найти в имени.",
            L"Ошибка",
            MB_OK | MB_ICONWARNING
        );

        return;
    }


    DWORD attributes =
        GetFileAttributesW(folder);


    if (
        attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)
    )
    {
        MessageBoxW(
            hwnd,
            L"Указанная папка не существует.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );

        return;
    }


    int process_files =
        Button_GetCheck(hFiles) ==
        BST_CHECKED;


    int process_directories =
        Button_GetCheck(hDirs) ==
        BST_CHECKED;


    int recursive =
        Button_GetCheck(hRecursive) ==
        BST_CHECKED;


    if (
        !process_files &&
        !process_directories
    )
    {
        MessageBoxW(
            hwnd,
            L"Выберите хотя бы один вариант: "
            L"файлы или папки.",
            L"Ошибка",
            MB_OK | MB_ICONWARNING
        );

        return;
    }


    /*
        Перед реальным переименованием
        спрашиваем подтверждение.
    */

    if (execute)
    {
        int result =
            MessageBoxW(
                hwnd,
                L"Выполнить переименование?\n\n"
                L"Рекомендуется сначала "
                L"использовать «Проверить».",
                L"Подтверждение",
                MB_YESNO |
                MB_ICONQUESTION |
                MB_DEFBUTTON2
            );


        if (result != IDYES)
            return;
    }


    /*
        Очищаем предыдущий результат.
    */

    SetWindowTextW(
        hLog,
        L""
    );


    g_scanned = 0;
    g_matched = 0;
    g_renamed = 0;
    g_errors = 0;


    status_update();


    if (execute)
        log_append(L"=== ВЫПОЛНЕНИЕ ===");
    else
        log_append(
            L"=== ПРЕДВАРИТЕЛЬНЫЙ ПРОСМОТР ==="
        );


    /*
        Запускаем рекурсивный обход.
    */

    walk_directory(
        folder,
        find,
        replace,
        execute,
        process_files,
        process_directories,
        recursive
    );


    status_update();

    log_append(L"=== ГОТОВО ===");
}


/* ---------------------------------------------------------
   Выбор каталога
   --------------------------------------------------------- */

static void choose_folder(HWND hwnd)
{
    BROWSEINFOW browse_info;

    ZeroMemory(
        &browse_info,
        sizeof(browse_info)
    );


    browse_info.hwndOwner = hwnd;

    browse_info.lpszTitle =
        L"Выберите папку для обработки";

    browse_info.ulFlags =
        BIF_RETURNONLYFSDIRS |
        BIF_NEWDIALOGSTYLE;


    PIDLIST_ABSOLUTE pid =
        SHBrowseForFolderW(
            &browse_info
        );


    if (pid != NULL)
    {
        wchar_t path[MAX_PATH];

        if (
            SHGetPathFromIDListW(
                pid,
                path
            )
        )
        {
            SetWindowTextW(
                hFolder,
                path
            );
        }

        CoTaskMemFree(pid);
    }
}


/* ---------------------------------------------------------
   Создание элемента интерфейса
   --------------------------------------------------------- */

static HWND create_control(
    HWND parent,
    const wchar_t *class_name,
    const wchar_t *text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id
)
{
    HWND control =
        CreateWindowExW(
            0,
            class_name,
            text,
            WS_CHILD |
            WS_VISIBLE |
            style,
            x,
            y,
            width,
            height,
            parent,
            (HMENU)(INT_PTR)id,
            GetModuleHandleW(NULL),
            NULL
        );


    SendMessageW(
        control,
        WM_SETFONT,
        (WPARAM)hFont,
        TRUE
    );


    return control;
}


/* ---------------------------------------------------------
   Главное окно
   --------------------------------------------------------- */

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (message)
    {
        case WM_CREATE:
        {
            hFont =
                (HFONT)GetStockObject(
                    DEFAULT_GUI_FONT
                );


            /* Папка */

            create_control(
                hwnd,
                L"STATIC",
                L"Папка для обработки:",
                0,
                15, 15,
                180, 20,
                0
            );


            hFolder =
                create_control(
                    hwnd,
                    L"EDIT",
                    L"",
                    WS_BORDER |
                    ES_AUTOHSCROLL,
                    15, 38,
                    625, 25,
                    IDC_FOLDER
                );


            create_control(
                hwnd,
                L"BUTTON",
                L"Выбрать...",
                BS_PUSHBUTTON,
                650, 37,
                110, 27,
                IDC_BROWSE
            );


            /* Что найти */

            create_control(
                hwnd,
                L"STATIC",
                L"Найти в имени:",
                0,
                15, 78,
                180, 20,
                0
            );


            hFind =
                create_control(
                    hwnd,
                    L"EDIT",
                    L"",
                    WS_BORDER |
                    ES_AUTOHSCROLL,
                    15, 101,
                    360, 25,
                    IDC_FIND
                );


            /* На что заменить */

            create_control(
                hwnd,
                L"STATIC",
                L"Заменить на "
                L"(пусто = удалить):",
                0,
                395, 78,
                300, 20,
                0
            );


            hReplace =
                create_control(
                    hwnd,
                    L"EDIT",
                    L"",
                    WS_BORDER |
                    ES_AUTOHSCROLL,
                    395, 101,
                    365, 25,
                    IDC_REPLACE
                );


            /* Файлы */

            hFiles =
                create_control(
                    hwnd,
                    L"BUTTON",
                    L"Файлы",
                    BS_AUTOCHECKBOX,
                    15, 142,
                    90, 22,
                    IDC_FILES
                );


            Button_SetCheck(
                hFiles,
                BST_CHECKED
            );


            /* Папки */

            hDirs =
                create_control(
                    hwnd,
                    L"BUTTON",
                    L"Папки",
                    BS_AUTOCHECKBOX,
                    115, 142,
                    90, 22,
                    IDC_DIRS
                );


            Button_SetCheck(
                hDirs,
                BST_CHECKED
            );


            /* Рекурсивно */

            hRecursive =
                create_control(
                    hwnd,
                    L"BUTTON",
                    L"Включая вложенные папки",
                    BS_AUTOCHECKBOX,
                    215, 142,
                    220, 22,
                    IDC_RECURSIVE
                );


            Button_SetCheck(
                hRecursive,
                BST_CHECKED
            );


            /* Проверка */

            create_control(
                hwnd,
                L"BUTTON",
                L"Проверить",
                BS_PUSHBUTTON,
                485, 137,
                125, 32,
                IDC_PREVIEW
            );


            /* Выполнение */

            create_control(
                hwnd,
                L"BUTTON",
                L"Выполнить замену",
                BS_DEFPUSHBUTTON,
                620, 137,
                140, 32,
                IDC_EXECUTE
            );


            /* Журнал */

            hLog =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_VSCROLL |
                    WS_HSCROLL |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_AUTOHSCROLL |
                    ES_READONLY,
                    15, 185,
                    745, 330,
                    hwnd,
                    (HMENU)IDC_LOG,
                    GetModuleHandleW(NULL),
                    NULL
                );


            SendMessageW(
                hLog,
                WM_SETFONT,
                (WPARAM)hFont,
                TRUE
            );


            /* Статус */

            hStatus =
                create_control(
                    hwnd,
                    L"STATIC",
                    L"Просмотрено: 0   "
                    L"Найдено: 0   "
                    L"Переименовано: 0   "
                    L"Ошибок: 0",
                    0,
                    15, 528,
                    745, 22,
                    IDC_STATUS
                );


            return 0;
        }


        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case IDC_BROWSE:
                    choose_folder(hwnd);
                    break;


                case IDC_PREVIEW:
                    run_job(
                        hwnd,
                        0
                    );
                    break;


                case IDC_EXECUTE:
                    run_job(
                        hwnd,
                        1
                    );
                    break;
            }

            return 0;
        }


        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }
    }


    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}


/* ---------------------------------------------------------
   Точка входа
   --------------------------------------------------------- */

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR commandLine,
    int showCommand
)
{
    (void)hPrevInstance;
    (void)commandLine;


    CoInitializeEx(
        NULL,
        COINIT_APARTMENTTHREADED
    );


    WNDCLASSEXW window_class;

    ZeroMemory(
        &window_class,
        sizeof(window_class)
    );


    window_class.cbSize =
        sizeof(window_class);

    window_class.lpfnWndProc =
        WindowProc;

    window_class.hInstance =
        hInstance;

    window_class.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW
        );

    window_class.hIcon =
        LoadIcon(
            NULL,
            IDI_APPLICATION
        );

    window_class.hbrBackground =
        (HBRUSH)(
            COLOR_BTNFACE + 1
        );

    window_class.lpszClassName =
        L"RecursiveRenameC";


    if (!RegisterClassExW(
            &window_class
        ))
    {
        CoUninitialize();
        return 1;
    }


    HWND hwnd =
        CreateWindowExW(
            0,
            window_class.lpszClassName,

            L"Рекурсивная замена "
            L"в именах файлов и папок",

            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,

            CW_USEDEFAULT,
            CW_USEDEFAULT,

            795,
            600,

            NULL,
            NULL,

            hInstance,
            NULL
        );


    if (hwnd == NULL)
    {
        CoUninitialize();
        return 1;
    }


    ShowWindow(
        hwnd,
        showCommand
    );

    UpdateWindow(hwnd);


    MSG message;


    while (
        GetMessageW(
            &message,
            NULL,
            0,
            0
        ) > 0
    )
    {
        TranslateMessage(
            &message
        );

        DispatchMessageW(
            &message
        );
    }


    CoUninitialize();

    return (int)message.wParam;
}
