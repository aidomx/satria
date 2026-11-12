# Contoh acuan DSL unit test — baca dulu sebelum menulis tes baru.
# Nama berkas: `.test` atau `.t`, keduanya sah dan setara (lihat example.t).
# Setiap baris komentar menjelaskan fitur di bawahnya.
@suite project
@purpose "Contoh acuan: fitur inti DSL dalam satu berkas"

# Aksi file: menulis proyek kecil di dir kerja (tersedia juga append/copy/
# touch/delete, work.new, fixture.copy, env.set, time.tick/mark).
file.write Buildfile <<T
use project
T
file.write src/main.c <<T
#include <stdio.h>
int main(void){ puts("halo"); return 0; }
T

# @for: rentang 1..3; {i} diinterpolasi ke argumen DAN isi heredoc.
# Catatan: {{...}} di C dibiarkan apa adanya — hanya {nama} yang diganti.
@for i in 1..3
file.append src/lib.c int f{i}(void){{return {i};}}
@end

# run.build: hasil per label; @response berisi asersi atas hasilnya.
# Mode project: nama binary = nama dir kerja (= nama berkas tes, tanpa
# ekstensi). Mode workspace: pakai selektor project(p).binary.
run.build as cold
@response cold
  rc.ok
  file.exists bin/example
@end

# run.exec: jalankan binary HASIL build; output/rc tersimpan per label.
run.exec bin/example as app
@response app
  rc.ok
  out.contains "halo"
@end

# @set + ternary; @get mencatat nilai ke log (tracing).
@set ok 1 < 2 ? ya : tidak
@get ok

# @if/@else: ekspresi L3 (predikat, pembanding, and/or/not).
@if exists(src/lib.c) and count(src/lib.c, ^int f) == 3
file.append verdict.txt semua-ada
@else
file.append verdict.txt kurang
@end
file.contains verdict.txt ^semua-ada$

# @while: polling sampai predikat salah (tanpa math: pakai predikat).
file.write w.txt <<T
1
T
@while lines(w.txt) < 3
file.append w.txt +1
@end
file.contains w.txt ^1$

# @find + @for koleksi: glob → daftar path → {var} tiap anggota.
@find c src/*.c
@for f in {c}
file.append daftar.txt {f}
@end
file.contains daftar.txt ^src/main.c$

# Escape literal \r \n \t di nilai dan pola.
file.append e.txt a\tb
file.contains e.txt ^a\tb$
