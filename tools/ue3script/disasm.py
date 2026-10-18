"""Prints the UnrealScript bytecode that --cherry_debug_dump_script_classes wrote, one statement per line.

Usage: python -X utf8 disasm.py <folder with script_bytecode.txt, script_objects.txt, script_names.txt> [Class.Function | Function | ~substring ...]

Native operators are named from the stock UE3 Object.uc indices and may be wrong for an index this game redefined; unknown ones print as nativeN.
"""
import sys, struct

objdir = sys.argv[1]
objs = {}
for line in open(objdir + '/script_objects.txt', encoding='utf-8', errors='replace'):
    parts = line.rstrip('\n').split(' ', 2)
    if len(parts) == 3:
        objs[int(parts[0], 16)] = (parts[1], parts[2])

names = {}
try:
    for line in open(objdir + '/script_names.txt', encoding='utf-8', errors='replace'):
        a, _, b = line.rstrip('\n').partition(' ')
        names[int(a)] = b
except FileNotFoundError:
    pass

NAT = {112:'$',114:'==O',119:'!=O',122:'==S',123:'!=S',125:'Len',129:'!',130:'&&',131:'^^',132:'||',133:'*=B',135:'+=B',136:'-=B',137:'++B',138:'--B',139:'B++',140:'B--',141:'~',143:'-I',144:'*',145:'/',146:'+',147:'-',148:'<<',149:'>>',150:'<',151:'>',152:'<=',153:'>=',154:'==',155:'!=',156:'&',157:'^',158:'|',159:'*=IF',161:'+=',162:'-=',163:'++I',164:'--I',165:'I++',166:'I--',167:'Rand',169:'-F',171:'*F',172:'/F',174:'+F',175:'-F',176:'<F',177:'>F',178:'<=F',179:'>=F',180:'==F',181:'!=F',184:'+=F',185:'-=F',242:'==Bool',243:'!=Bool',249:'Min',250:'Max',251:'Clamp',254:'==N',255:'!=N'}


class Dis:
    def __init__(self, code):
        self.c = code
        self.p = 0
        self.out = []

    def b(self):
        v = self.c[self.p]; self.p += 1; return v

    def w(self):
        v = struct.unpack('>H', self.c[self.p:self.p + 2])[0]; self.p += 2; return v

    def i(self):
        v = struct.unpack('>i', self.c[self.p:self.p + 4])[0]; self.p += 4; return v

    def f(self):
        v = struct.unpack('>f', self.c[self.p:self.p + 4])[0]; self.p += 4; return v

    def ptr(self):
        hi, lo = struct.unpack('>II', self.c[self.p:self.p + 8]); self.p += 8
        a = lo if hi == 0 else hi
        if a == 0: return 'None'
        o = objs.get(a)
        return o[1].split('.')[-1] if o else '?%08X' % a

    def name(self):
        idx, num = struct.unpack('>II', self.c[self.p:self.p + 8]); self.p += 8
        return names.get(idx, 'name#%d' % idx)

    def params(self):
        args = []
        while self.c[self.p] != 0x16:
            args.append(self.expr())
        self.p += 1
        return args

    def expr(self):
        op = self.b()
        if op in (0x00, 0x01, 0x02, 0x03, 0x48): return self.ptr()
        if op == 0x04: return 'return ' + self.expr()
        if op == 0x05:
            self.ptr(); self.b(); return 'switch(%s)' % self.expr()
        if op == 0x06: return 'jump %04X' % self.w()
        if op == 0x07:
            t = self.w(); return 'if !(%s) jump %04X' % (self.expr(), t)
        if op == 0x08: return 'stop'
        if op == 0x09:
            self.w(); self.b(); return 'assert(%s)' % self.expr()
        if op == 0x0A:
            t = self.w()
            return 'default:' if t == 0xFFFF else 'case %s: (next %04X)' % (self.expr(), t)
        if op == 0x0B: return 'nothing'
        if op == 0x0D: return 'goto ' + self.expr()
        if op == 0x0E: return 'eat(%s)' % self.ptr()
        if op == 0x0F: a = self.expr(); return '%s = %s' % (a, self.expr())
        if op == 0x14: a = self.expr(); return '%s = %s' % (a, self.expr())
        if op == 0x44: a = self.expr(); return '%s = %s' % (a, self.expr())
        if op in (0x10, 0x1A): i = self.expr(); a = self.expr(); return '%s[%s]' % (a, i)
        if op == 0x11: return 'new(%s)' % ', '.join(self.expr() for _ in range(5))
        if op in (0x12, 0x19):
            o = self.expr(); self.w(); self.ptr(); self.b(); return '%s.%s' % (o, self.expr())
        if op in (0x13, 0x2E, 0x52): c = self.ptr(); return '%s(%s)' % (c, self.expr())
        if op == 0x15: return '<endparm>'
        if op == 0x16: return '<endparms>'
        if op == 0x17: return 'self'
        if op == 0x18: self.w(); return self.expr()
        if op in (0x1B, 0x37): n = self.name(); return '%s(%s)' % (n, ', '.join(self.params()))
        if op == 0x1C: n = self.ptr(); return '%s(%s)' % (n, ', '.join(self.params()))
        if op == 0x1D: return str(self.i())
        if op == 0x1E: return '%gf' % self.f()
        if op == 0x1F:
            e = self.c.index(0, self.p); s = self.c[self.p:e].decode('latin1'); self.p = e + 1; return repr(s)
        if op == 0x20: return self.ptr()
        if op == 0x21: return "'%s'" % self.name()
        if op in (0x22, 0x23): self.p += 12; return '<vec>'
        if op == 0x24: return 'b%d' % self.b()
        if op == 0x25: return '0'
        if op == 0x26: return '1'
        if op == 0x27: return 'true'
        if op == 0x28: return 'false'
        if op == 0x29: return self.ptr()
        if op == 0x2A: return 'None'
        if op == 0x2C: return str(self.b())
        if op == 0x2D: return self.expr()
        if op == 0x2F: e = self.expr(); self.w(); return 'foreach ' + e
        if op in (0x30, 0x31): return 'iter%X' % op
        if op in (0x32, 0x33): self.ptr(); a = self.expr(); return '(%s %s %s)' % (a, '==' if op == 0x32 else '!=', self.expr())
        if op == 0x34:
            s = ''
            while True:
                ch = self.w()
                if not ch: break
                s += chr(ch)
            return repr(s)
        if op == 0x35:
            prop = self.ptr(); self.ptr(); self.b(); self.b(); return '%s.%s' % (self.expr(), prop)
        if op == 0x36: return '%s.Length' % self.expr()
        if op == 0x38: t = self.b(); return 'cast%d(%s)' % (t, self.expr())
        if op in (0x39, 0x40): a = self.expr(); return '%s.%s(%s)' % (a, 'Insert' if op == 0x39 else 'Remove', ', '.join(self.params()))
        if op == 0x3A: self.ptr(); return 'returnnothing'
        if op in (0x3B, 0x3C, 0x3D, 0x3E): return 'delcmp(%s)' % ', '.join(self.params())
        if op == 0x3F: return 'emptydelegate'
        if op == 0x41: self.p += 13; return 'debuginfo'
        if op == 0x42: self.b(); p = self.ptr(); self.name(); return 'delegate %s(%s)' % (p, ', '.join(self.params()))
        if op == 0x43: n = self.name(); self.ptr(); return 'delprop ' + n
        if op == 0x45:
            c = self.expr(); self.w(); a = self.expr(); self.w(); return '(%s ? %s : %s)' % (c, a, self.expr())
        if op == 0x46: a = self.expr(); self.w(); return '%s.Find(%s)' % (a, ', '.join(self.params()))
        if op == 0x47: a = self.expr(); self.w(); return '%s.FindStruct(%s)' % (a, ', '.join(self.params()))
        if op == 0x49: self.w(); e = self.expr(); self.b(); return 'defparm(%s)' % e
        if op == 0x4A: return '<empty>'
        if op == 0x4B: return 'instdelegate ' + self.name()
        if op == 0x51: return 'iface(%s)' % self.expr()
        if op == 0x53: return '<end>'
        if op in (0x54, 0x55, 0x56): a = self.expr(); self.w(); return '%s.%s(%s)' % (a, {0x54: 'Add', 0x55: 'AddItem', 0x56: 'RemoveItem'}[op], ', '.join(self.params()))
        if op == 0x57: a = self.expr(); self.w(); return '%s.InsertItem(%s)' % (a, ', '.join(self.params()))
        if op == 0x58: a = self.expr(); b = self.expr(); u = self.b(); c = self.expr() if u else ''; self.w(); return 'foreach %s(%s, %s)' % (a, b, c)
        if 0x60 <= op < 0x70:
            idx = ((op - 0x60) << 8) | self.b(); return 'native%d(%s)' % (idx, ', '.join(self.params()))
        if op >= 0x70:
            return '%s(%s)' % (NAT.get(op, 'native%d' % op), ', '.join(self.params()))
        raise Exception('unknown op %02X at %04X' % (op, self.p - 1))

    def run(self):
        while self.p < len(self.c):
            at = self.p
            try:
                e = self.expr()
            except Exception as ex:
                self.out.append('%04X !! %s' % (at, ex)); break
            self.out.append('%04X %s' % (at, e))
            if e == '<end>': break
        return self.out


want = sys.argv[2:]
for line in open(objdir + '/script_bytecode.txt', encoding='utf-8', errors='replace'):
    parts = line.split()
    if len(parts) < 2: continue
    path = parts[0]
    if want and not any(w == path or path.endswith('.' + w) or (w.startswith('~') and w[1:] in path) for w in want): continue
    print('==== ' + path)
    for l in Dis(bytes.fromhex(parts[1])).run(): print('  ' + l)
