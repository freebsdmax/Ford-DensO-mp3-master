p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()

# Remove everything from the broken JS we just added
# Look for the pattern we just inserted
start = t.find('function loadElm327Cfg()')
if start >= 0:
    # Find start of line with just quote before function?
    # Look backwards for \n" 
    # easier: cut from start back to find line start of broken string
    # actually look for \n"\nfunction
    marker = '\n"\nfunction loadElm327Cfg()'
    if marker in t:
        s2 = t.find(marker)
        # cut until </script>"
        endm = t.find('</script>"', s2)
        if endm >= 0:
            endm2 = endm + len('</script>"')
            t2 = t[:s2] + '\n' + t[endm2:]
            open(p,'w',encoding='utf-8').write(t2)
            print('fixed')
            exit()
    
# Try different patterns
marker2 = 'function loadElm327Cfg()'
s = t.find(marker2)
if s >= 0:
    # go back to find beginning of that line's string
    # look for \n    " or \n"
    # cut backwards to previous proper content
    # easier: just remove from s to before </script>
    end = t.find('</script>', s)
    if end >= 0:
        # go back to include quote start
        # find last " before s on same context
        # cut from \n" before s
        pre = t.rfind('\n"', 0, s)
        if pre >= 0 and pre > s - 200:
            t2 = t[:pre] + '\n</script>' + t[end+9:]
            open(p,'w',encoding='utf-8').write(t2)
            print('fixed2')
            exit()
print('no')
