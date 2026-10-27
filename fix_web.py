p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# remove broken script I added outside string
# look for function definitions outside
# just cut from "<script>" added by me if outside INDEX_HTML
# easier: find first occurrence of "<script>" not after "INDEX_HTML" string start? 
# just remove script block I added
start = t.find("<script>\nfunction loadElm327")
if start >= 0:
    end = t.find("</script>", start)
    if end >= 0:
        end2 = end + len("</script>")
        # check if it's inside quotes roughly
        # remove
        t2 = t[:start] + t[end2:]
        open(p,'w',encoding='utf-8').write(t2)
        print('removed')
    else:
        print('noend')
else:
    print('nos')
