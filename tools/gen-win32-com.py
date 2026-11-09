"""Generate only COM ABI metadata/wrappers from Microsoft's SDK declarations.
Usage: gen-win32-com.py sdk-header output prefix Name:kind [...]
No implementation code is imported.
"""
import pathlib,re,sys,struct
source=pathlib.Path(sys.argv[1]).read_text()
output=sys.argv[2];prefix=sys.argv[3]
interfaces=[];operations=[]
for specification in sys.argv[4:]:
    name,kind=specification.split(':');kind=int(kind)
    body=re.search(r'DECLARE_INTERFACE_\(\s*'+name+r'\s*,[^)]*\)\s*\{(.*?)\n\};',source,re.S).group(1)
    methods=[]
    pattern=r'STDMETHOD(?:_\([^,]+,\s*(\w+)\)|\((\w+)\))\s*\((.*?)\)\s*PURE'
    for m in re.finditer(pattern,body,re.S):
        method=m[1] or m[2];args=m[3];depth=0;commas=0
        for c in args:
            if c=='(':depth+=1
            elif c==')':depth-=1
            elif c==',' and not depth:commas+=1
        count=1 if args.strip()=='THIS' else commas+2 # THIS_ expands to this + first argument
        if method not in operations:operations.append(method)
        methods.append((method,count))
    guid_match=re.search(r'DEFINE_GUID\(\s*IID_'+name+r'\s*,([^;]+)\);',source)
    nums=[int(x.strip(),0) for x in guid_match[1].split(',')]
    guid=struct.pack('<IHH8B',*nums)
    interfaces.append((name,kind,methods,guid))
lines=['/* Generated COM ABI metadata; implementation lives in '+prefix+'.c. */']
lines+=['enum {'+', '.join(prefix.upper()+'OP_'+m for m in operations)+'};']
for name,kind,methods,guid in interfaces:
    for method,count in methods:
        lines.append(f'DEF({prefix}{kind}_{method}) {{return {prefix}_dispatch({kind},{prefix.upper()}OP_{method},a);}}')
lines.append(f'static const struct api_entry {prefix}_methods[]={{')
for name,kind,methods,guid in interfaces:
    for method,count in methods:lines.append(f'    API({prefix}{kind}_{method},{count}),')
lines+=['};',f'static const struct com_spec {prefix}_specs[]={{']
start=0
for name,kind,methods,guid in interfaces:
    lines.append('    {'+f'{kind},{start},{len(methods)},'+ '{'+','.join(hex(x) for x in guid)+'}},')
    start+=len(methods)
lines.append('};')
pathlib.Path(output).write_text('\n'.join(lines)+'\n')
print([(name,len(methods)) for name,_,methods,_ in interfaces])
