p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# remove broken lines
# find line with just </div></div>
# and next </body></html>";
bad1 = '    </div></div>\n</body></html>";\n'
if bad1 in t:
    t2 = t.replace(bad1, '    </div>\n</div>\n</body></html>";\n')
    open(p,'w',encoding='utf-8').write(t2)
    print('ok1')
    exit()
# try different
bad2 = '</div></div>\n</body></html>";'
if bad2 in t:
    t2 = t.replace(bad2, '</div>\n</div>\n</body></html>";')
    open(p,'w',encoding='utf-8').write(t2)
    print('ok2')
    exit()
print('no')
