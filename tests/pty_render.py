import sys, re, argparse

ap = argparse.ArgumentParser()
ap.add_argument('raw')
ap.add_argument('--size', default='80x24')
a = ap.parse_args()
W, H = (int(x) for x in a.size.split('x'))
scr = [[' '] * W for _ in range(H)]
r = c = 0

data = open(a.raw, 'rb').read().decode('latin-1')
i = 0
n = len(data)
CSI = re.compile(r'\x1b\[([0-9;?]*)([\x40-\x7e])')
while i < n:
    ch = data[i]
    if ch == '\x1b':
        m = CSI.match(data, i)
        if m:
            args, cmd = m.group(1), m.group(2)
            nums = [int(x) for x in args.split(';') if x.isdigit()]
            if cmd == 'H' or cmd == 'f':
                r = (nums[0] - 1) if len(nums) > 0 and nums[0] else 0
                c = (nums[1] - 1) if len(nums) > 1 and nums[1] else 0
            elif cmd == 'd':
                r = (nums[0] - 1) if nums else 0
            elif cmd == 'G' or cmd == '`':
                c = (nums[0] - 1) if nums else 0
            elif cmd == 'A':
                r = max(0, r - (nums[0] if nums else 1))
            elif cmd == 'B':
                r = min(H - 1, r + (nums[0] if nums else 1))
            elif cmd == 'C':
                c = min(W - 1, c + (nums[0] if nums else 1))
            elif cmd == 'D':
                c = max(0, c - (nums[0] if nums else 1))
            elif cmd == 'M':
                r = max(0, r - 1)   # RI: reverse index (no scroll model)
            elif cmd == 'L':
                k = nums[0] if nums else 1   # IL: insert k blank lines at r
                for _ in range(k):
                    scr.insert(r, [' '] * W)
                del scr[H:]
            elif cmd == 'P':
                k = nums[0] if nums else 1   # DCH
                row = scr[r]
                del row[c:c + k]
                row.extend([' '] * k)
            elif cmd == '@':
                k = nums[0] if nums else 1   # ICH
                row = scr[r]
                for _ in range(k):
                    row.insert(c, ' ')
                del row[W:]
            elif cmd == 'X':
                k = nums[0] if nums else 1   # ECH
                for x in range(c, min(W, c + k)):
                    scr[r][x] = ' '
            elif cmd == 'J':
                a0 = nums[0] if nums else 0
                if a0 == 2:
                    scr = [[' '] * W for _ in range(H)]
                elif a0 == 1:
                    for y in range(0, r + 1):
                        lim = W if y < r else c + 1
                        for x in range(0, lim):
                            scr[y][x] = ' '
                else:
                    for x in range(c, W):
                        scr[r][x] = ' '
                    for y in range(r + 1, H):
                        scr[y] = [' '] * W
            elif cmd == 'K':
                a0 = nums[0] if nums else 0
                if a0 == 2:
                    for x in range(W):
                        scr[r][x] = ' '
                elif a0 == 1:
                    for x in range(0, c + 1):
                        scr[r][x] = ' '
                else:
                    for x in range(c, W):
                        scr[r][x] = ' '
            i = m.end()
            continue
        m = re.match(r'\x1b\][^\x07]*(\x07|\x1b\\)?', data[i:])   # OSC
        if m and m.end() > 0:
            i += m.end()
            continue
        m = re.match(r'\x1b\(B', data[i:])
        if m:
            i += m.end()
            continue
        m = re.match(r'\x1b[=><M78]', data[i:])
        if m:
            i += m.end()
            continue
        i += 1
        continue
    if ch == '\r':
        c = 0
    elif ch == '\n':
        r = (r + 1) % H
    elif ch == '\x08':
        c = max(0, c - 1)
    elif ch == '\t':
        c = min(W - 1, (c // 8 + 1) * 8)
    elif 32 <= ord(ch) < 127:
        if r < H and c < W:
            scr[r][c] = ch
        c += 1
    i += 1

for row in scr:
    sys.stdout.write(''.join(row).rstrip() + '\n')
