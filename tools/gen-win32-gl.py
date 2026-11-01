"""Generate stack-ABI wrappers for the native fluxOS GL API, not native pointers."""
import pathlib, re, sys
source = pathlib.Path(sys.argv[1]).read_text()
source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
entries = []
output = ['/* Generated from GL/gl.h by tools/gen-win32-gl.py. */']
for result, name, args in re.findall(r'([\w\s*]+?)\b(gl\w+)\s*\(([^;]*?)\)\s*;', source):
    result = result.strip()
    if result.startswith('typedef') or name in ('glGetString', 'glGetPointerv'):
        continue
    result = result.split('\n')[-1].strip()
    if result not in ('void','GLboolean','GLenum','GLuint','GLint'):
        raise ValueError((result,name))
    converted = []
    words = 0
    for arg in args.split(',') if args.strip() not in ('','void') else []:
        arg = arg.strip()
        typ = re.sub(r'\b\w+$', '', arg).strip()
        if '*' in typ:
            converted.append(f'({typ})gp(a[{words}])'); words += 1
        elif typ in ('GLdouble','GLclampd'):
            converted.append(f'arg_double(a+{words})'); words += 2
        elif typ in ('GLfloat','GLclampf'):
            converted.append(f'arg_float(a[{words}])'); words += 1
        else:
            converted.append(f'({typ})a[{words}]'); words += 1
    call = f'{name}({", ".join(converted)})'
    body = f'{call};return 0;' if result=='void' else f'return {call};'
    output.append(f'DEF({name}) {{(void)a;if(!gl_current()) return 0;{body}}}')
    entries.append(f'API({name},{words}),')
output.append('#define WIN_GL_ENTRIES \\\n    ' + ' \\\n    '.join(entries))
pathlib.Path(sys.argv[2]).write_text('\n'.join(output)+'\n')
print(f'Generated {len(entries)} GL wrappers')
