Import("env")
import os, shutil

project_dir = env.subst("$PROJECT_DIR")
libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
pioenv = env.subst("$PIOENV")

src = os.path.join(project_dir, "include", "lv_conf.h")
dst_dir = os.path.join(libdeps_dir, pioenv)
dst = os.path.join(dst_dir, "lv_conf.h")

if os.path.isdir(dst_dir) and os.path.isfile(src) and not os.path.isfile(dst):
    shutil.copy(src, dst)
    print(f"Copied lv_conf.h -> {dst}")