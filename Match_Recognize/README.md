# Row Pattern Match

## Required Packages
```sh
# Linux
$ sudo build_support/packages.sh
# macOS
$ build_support/packages.sh
# boost
$ sudo apt install libboost-all-dev
```

## How to compile
```sh
mkdir build
cd build
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=YES DCMAKE_BUILD_TYPE=Debug ..
make -j`nproc`
```

## Formatting
The code follows the Google C++ Style Guide.
```sh
# inside build 
make check-format
make check-lint
```