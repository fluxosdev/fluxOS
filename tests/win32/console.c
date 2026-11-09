/* Console regressions also exercise SHLWAPI exports through the real guest
 * GetProcAddress thunks, including guest-pointer and UTF-16 handling. */
#include "winapi.h"
#define CHECK(x) do { if (!(x)) W(ExitProcess)(1); } while (0)
typedef char *(WINAPI *find_a)(const char *);
typedef unsigned short *(WINAPI *find_w)(const unsigned short *);
typedef BOOL (WINAPI *relative_a)(const char *);
typedef BOOL (WINAPI *relative_w)(const unsigned short *);
typedef void (WINAPI *remove_a)(char *);
typedef void (WINAPI *remove_w)(unsigned short *);

void start(void)
{
    CHECK(W(GetModuleHandleA)(0) == (HMODULE)0x400000);
    HMODULE sh = W(GetModuleHandleA)("shlwapi.dll");
    CHECK(sh);
    find_a filename = (find_a)W(GetProcAddress)(sh, "PathFindFileNameA");
    find_w filenamew = (find_w)W(GetProcAddress)(sh, "PathFindFileNameW");
    find_a extension = (find_a)W(GetProcAddress)(sh, "PathFindExtensionA");
    find_w extensionw = (find_w)W(GetProcAddress)(sh, "PathFindExtensionW");
    remove_a remove = (remove_a)W(GetProcAddress)(sh, "PathRemoveExtensionA");
    remove_w removew = (remove_w)W(GetProcAddress)(sh, "PathRemoveExtensionW");
    relative_a relative = (relative_a)W(GetProcAddress)(sh, "PathIsRelativeA");
    relative_w relativew = (relative_w)W(GetProcAddress)(sh, "PathIsRelativeW");
    CHECK(filename && filenamew && extension && extensionw && remove && removew && relative && relativew);
    char path[] = "C:\\dir.ext\\file.tar.gz";
    CHECK(filename(path) == path + 11);
    CHECK(extension(path) == path + 19);
    remove(path);
    CHECK(!path[19] && extension(path) == path + 15);
    char plain[] = "dir.ext\\file", space[] = "file.ext more", empty[] = "";
    CHECK(extension(plain) == plain + 12);
    CHECK(extension(space) == space + 13);
    CHECK(filename(empty) == empty && extension(empty) == empty);
    char dot[] = ".hidden", trailing[] = "file.";
    CHECK(extension(dot) == dot && extension(trailing) == trailing + 4);
    remove(dot); CHECK(!dot[0]);
    remove(plain); CHECK(plain[11] == 'e' && !plain[12]);
    CHECK(relative("") && relative("..\\file") && !relative("C:foo") && !relative("\\file"));
    CHECK(!relative("\\\\server\\share"));
    unsigned short wide[] = {'C',':','\\',0x4e2d,'\\',0x6587,'.','t','x','t',0};
    CHECK(filenamew(wide) == wide + 5 && extensionw(wide) == wide + 6);
    CHECK(!relativew(wide) && relativew(wide + 5));
    removew(wide); CHECK(!wide[6] && extensionw(wide) == wide + 6 && wide[5] == 0x6587);
    HMODULE user = W(GetModuleHandleA)("user32.dll");
    typedef UINT (WINAPI *register_a)(const char *);
    typedef UINT (WINAPI *register_w)(const unsigned short *);
    register_a reg = (register_a)W(GetProcAddress)(user, "RegisterWindowMessageA");
    register_w regw = (register_w)W(GetProcAddress)(user, "RegisterWindowMessageW");
    CHECK(reg && regw);
    UINT message = reg("flux-test-message");
    CHECK(message >= 0xc000 && message <= 0xffff);
    CHECK(reg("flux-test-message") == message);
    CHECK(reg("FLUX-TEST-MESSAGE") == message);
    CHECK(regw((const unsigned short *)u"flux-test-message") == message);
    UINT other = reg("flux-test-message-other");
    CHECK(other >= 0xc000 && other <= 0xffff && other != message);
    CHECK(regw((const unsigned short *)u"flux-test-message-other") == other);
    CHECK(!reg(0) && !reg(""));
    W(ExitProcess)(5);
}
