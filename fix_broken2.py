p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# replace from "</div>" alone to proper close
old = '    "</div>\n    </div>\n"</body></html>";\n'
if old in t:
    t2 = t.replace(old, '    </div>\n</div>\n</body></html>";\n')
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
    exit()
old2 = '    "</div>\n</div>\n</body></html>";'
if old2 in t:
    t2 = t.replace(old2, '    </div>\n</div>\n</body></html>";')
    open(p,'w',encoding='utf-8').write(t2)
    print('ok2')
    exit()
print('no')
