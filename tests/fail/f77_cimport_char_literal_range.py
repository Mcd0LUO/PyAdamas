# expect: literal 200 is out of range for 'i8'
cimport cfixrange as f
f.char_id(200)
