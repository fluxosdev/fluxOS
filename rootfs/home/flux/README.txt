Welcome to fluxOS 0.3.0
=======================

fluxOS is an experimental hobby operating system, built with AI assistance
from Claude, Google Gemini, and ChatGPT Codex. Run it in a virtual machine
or on a computer with nothing on it you would miss, and do not keep anything
you care about on it. It has no security model: one user, no firewall.

fluxOS runs from the hard disk Setup installed it on, so what you save stays
across reboots; only /tmp is kept in memory. It needs a Pentium III or
anything newer with SSE, and uses up to 4 GiB of RAM. Extra processors and
cores run programs too ("nosmp" on the boot command line turns that off).


The desktop
-----------
  Start menu       the Start button, the Windows key or Ctrl+Esc
  Alt+Tab          switch windows;  Alt+F4 closes one
  Windows          drag the title bar to move, any edge or corner to resize,
                   double-click the title bar to maximize
  Sidebar          down the right edge on screens 1024 pixels wide or more:
                   a clock, a calendar and a graph of processor and memory
                   use. Right-click it to add or remove widgets, and
                   right-click the taskbar to show or hide it.
  Screen saver     a drifting logo, after the desktop is left alone for a
                   while (Display Properties > Screen Saver)

  Display Properties (Start menu > Settings)
                   Background: wallpaper (Fill, Stretch, Center, Tile) or a
                   plain color; Appearance: the Kinetik theme in six colors,
                   or Classic, the Windows 95 look; video mode, monitor and
                   refresh rate where the video driver can change them


Programs
--------
  Terminal         a shell (fsh) in a window; open as many as you like
  File Manager     folders, disks and pictures; cut, copy and paste between
                   disks, rename, "Open Terminal Here", and double-click to
                   open things: pictures, text, sounds, web pages, archives
  Text Editor      this file is open in it (no undo or search, and files up
                   to 1 MB)
  Wave             the web browser, with tabs, HTTP and HTTPS (see below)
  Paint            draw on a canvas up to 2048 x 2048 with brush, eraser,
                   fill, line and pick tools; opens PNG, JPEG, GIF and SVG;
                   saves JPEG; Ctrl+Z undoes
  Archive Manager  browse ZIP and 7z archives like folders, and extract them
                   with a progress bar (double-click one in the File Manager)
  Calculator       exact decimal arithmetic
  Sound Player     plays WAV files, with pause and a seek bar
  3D Gears         the classic OpenGL demo (arrow keys or drag to turn it,
                   Space pauses, S switches between hardware and software,
                   T puts a texture on the gears)
  Minesweeper      what it says
  Task Manager     programs, processes, and processor and memory graphs
  Disk Manager     volumes and partitions: mount, unmount, format, edit the
                   partition table
  Volume Control   click the speaker in the taskbar
  Date and Time, System Properties
                   in the Start menu under Settings


Wave, the web browser
---------------------
  Ctrl+T / Ctrl+W      new tab / close tab (Ctrl+Tab or Ctrl+1..9 to switch)
  Ctrl+L, F6           the address bar;  F5 reloads;  Esc stops
  Ctrl+F               find on the page (Enter for the next match)
  Ctrl++  Ctrl+-  Ctrl+0   zoom from 30% to 300%
  Ctrl+D               add to Favorites (the Favorites button lists them)
  Ctrl+P               print the page to a JPEG in ~/Downloads
  Alt+Left, Alt+Right  back and forward;  right-click for the page's menu

  Pages come over HTTP and HTTPS. HTTPS is TLS 1.3 only, and the server's
  certificate is checked against /etc/ssl/ca-certificates.crt; a connection
  that fails the check is refused, not warned about. Cookies, localStorage,
  downloads, favicons and WebSocket (ws:// and wss://) work, and pages
  load faster for a disk cache in ~/.cache/http.

  It runs JavaScript (modules, async/await, classes, Promises, fetch,
  custom elements, shadow DOM...), modern CSS (flexbox, grid, custom
  properties, :has(), nesting, cascade layers, 2D transforms), and PNG,
  JPEG, GIF, WebP and SVG pictures. Real sites can still break: no Web
  Workers, IndexedDB or 3D transforms, and CSS transitions are ignored.

  Try the test pages: file:///usr/share/www/home.html links to them. Simple
  sites such as 68k.news, wiby.me and textfiles.com suit it best.


Pictures and sound
------------------
  JPEG, PNG, GIF and SVG pictures open in an image viewer, and JPEG and PNG
  can be your wallpaper: put them in /usr/share/wallpapers or your home
  folder and they appear in Display Properties > Background.

  WAV and VOC files play when you double-click them. In a terminal:
    play /usr/share/sounds/startup.wav   the Sound Blaster 16 or HD Audio
    play -t 440                          a test tone
    beep -m "T160 L8 CDEGEC4"            the PC speaker, playing a tune
    mixer; mixer pcm 60                  show and set the volumes


Disks
-----
  CDs, floppy disks and USB sticks are mounted by themselves when you put
  them in, on /mnt/cdrom, /mnt/fd0 and so on, and unmounted when you take
  them out. A disk you unmount yourself (umount /mnt/fd0) stays unmounted
  until it is removed. Hard disks are on /mnt/hda, /mnt/hdb... (lsblk lists
  them). IDE, SATA (AHCI) and USB storage work; FAT12/16/32 can be written,
  ISO9660 only read.

    ls /mnt/cdrom           what is on the disc
    cp notes.txt /mnt/fd0   copy to the floppy;  sync   writes it out now
    dmesg | grep mounted    what the automounter did

  Unmount a floppy or USB stick (umount /mnt/fd0) before pulling it out:
  changes that have not been written yet are lost with it.

  To install fluxOS on another hard disk, start the computer from the fluxOS
  CD (legacy BIOS boot): its Setup wizard erases the disk you pick and
  installs onto it. Disk Manager in Setup lets you look around first, and
  Shift+F10 opens a command prompt. From a terminal, "fluxinstall /dev/hda"
  does the same on the console.

  The installed system's boot menu also has a text console, "show boot
  messages", and a safe mode (one processor, the BIOS display mode, no
  Radeon, Intel or Voodoo driver) for when something does not start.


Laptops and power
-----------------
  power              battery charge and time left, AC adapter, lid, and
                     temperature
  power bright +     the screen brighter (- dimmer, or a percentage); the
                     brightness keys work too
  power sleep        suspend to RAM. This is experimental: it works in QEMU
                     but is not yet reliable on real hardware, so save your
                     work first. "lid=ignore" on the kernel command line
                     stops closing the lid from sleeping the machine.
  Synaptics touchpads tap, drag and scroll; USB keyboards, mice and tablets
  work, and the volume and sleep keys reach the desktop.


Video, sound and network cards
------------------------------
  Graphics   3dfx Voodoo Banshee/3/4/5 and ATI Radeon HD 4000 (changeable
             modes and a 3D engine for 3D Gears), Intel HD Graphics of
             Haswell processors and Radeon HD 6000 of Llano APUs (smooth
             page flipping and a hardware pointer in the mode the boot
             loader chose). Anything else keeps that mode.
  Sound      Sound Blaster 16, Intel HD Audio, and the PC speaker
  Network    NE2000, nVidia nForce and Intel I217 gigabit; an address comes
             from DHCP by itself


Multitasking
------------
  spin A 5 &         a CPU-bound task in the background (never yields)
  spin B 5 &         ...and another; the shell stays responsive
  jobs               list background jobs, 'fg %1' brings one back
  top                live CPU usage per process (^C to quit)
  ps                 the process table


The shell (fsh) is zsh-flavoured
--------------------------------
  * syntax highlighting: commands turn green when they exist
  * grey autosuggestions from history; Right arrow or ^E accepts
  * Tab completion with zsh-style listings
  * Up/Down search history by the prefix you typed
  * AUTO_CD: type a directory name to cd into it
  * globs that match nothing are an error ("no matches found")
  * continuation prompts: if> for> dquote> pipe> ...

  for i in $(seq 1 3); do echo "round $i"; done
  if [ -d /bin ]; then echo yes; else echo no; fi
  greet() { echo "hello, ${1:-world}"; }; greet flux
  echo $((6 * 7))
  ./demo.sh                     a longer scripting demo
  ls /bin | grep -c .           pipes and redirection
  sort < /etc/fshrc | uniq | head -3

  Terminals are 80x25 with no scrollback, so pipe long output into "more".


Network
-------
  ifconfig; ping -c 3 10.0.2.2     the network card and its gateway
  host example.com                 DNS
  wget -O - https://example.com/   HTTP and HTTPS (certificates checked)
  nc -l 7777 &  echo hi | nc localhost 7777


Archives
--------
  unzip -l a.zip; unzip a.zip      list and extract ZIP and 7z archives
  7z x a.7z                        the same with 7-Zip's commands
  zip -r a.zip folder              make a ZIP


DOS programs
------------
  ./GAME.EXE         .COM and .EXE files run in a terminal (also: dos game)
                     with sound: PC speaker, AdLib and Sound Blaster 16
  /dos               the C: drive: C:\GAMES\DOOM is /dos/GAMES/DOOM
  ls /mnt/fd0        the floppy disk in drive A: (DOS programs see it as A:)
  dos -l /tmp/dos.log GAME.EXE   log what the emulator cannot do
  Ctrl+\             stops a DOS program that will not stop
  Text mode and VGA graphics work; DOOM-style DOS extenders do too.


System
------
  free, uname -a, uptime, date, dmesg, fetch, lsblk, lsusb, df -h
  reboot, poweroff   (both write out what is waiting for the disk first)

  If the kernel itself fails, a panic screen appears over the desktop;
  press Enter to restart the computer.


More about fluxOS is in README.md in the source tree, including the full
list of what works, what does not, and how to build it. Credits: the default
wallpaper is a photograph by kazuend on Unsplash (and mountains.jpg is by
Stefan Stefancik); the icons are from KDE's Oxygen theme; the Kinetik
theme's text is set in Archivo (SIL Open Font License, in
/usr/share/doc/fonts/LICENSE.archivo).
