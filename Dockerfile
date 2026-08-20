FROM ubuntu:22.04 AS builder_loupe

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# Install required packages (base + Qt dependencies)
RUN apt update && apt install -y \
    git build-essential cmake curl unzip ninja-build \
    g++ wget pkg-config make zip tar python3 nano sudo \
    software-properties-common ca-certificates gnupg \
    lsb-release libfontconfig-dev libpulse-dev libpulse0 \
    libpipewire-0.3-0 libxcb-cursor0 libxcb-xinerama0 \
    libxkbcommon-x11-0 libx11-xcb1 libxcb1 libxcb-render0 \
    libxcb-shape0 libxcb-shm0 libopengl-dev libgl1-mesa-dev \
    libxcb-icccm4 libxcb-util1 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-xfixes0 \
    libxcb-sync1 libxcb-xkb1 python3 python3-pip \
    && apt clean

# Clone and bootstrap vcpkg
WORKDIR /opt
RUN git clone https://github.com/Microsoft/vcpkg.git
WORKDIR /opt/vcpkg
RUN ./bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

# Clone LOUPE
WORKDIR /root
RUN git clone https://github.com/JakubMelka/PDF4QT.git
ENV VCPKG_OVERLAY_PORTS=/root/LOUPE/vcpkg/overlays

RUN pip install aqtinstall

RUN aqt list-qt linux desktop --arch 6.11.1 > /root/available_qt_versions.txt

RUN aqt install-qt linux desktop 6.11.1 linux_gcc_64 -O /opt/Qt -m qtmultimedia qtspeech

# Set environment variables
ENV PATH=/opt/Qt/6.11.1/gcc_64/bin:$PATH
ENV CMAKE_PREFIX_PATH=/opt/Qt/6.11.1/gcc_64/lib/cmake
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_OVERLAY_PORTS=/root/LOUPE/vcpkg/overlays
ENV LOUPE_QT_ROOT=/opt/Qt/6.11.1/gcc_64
ENV LD_LIBRARY_PATH=/opt/Qt/6.11.1/gcc_64/lib:$LD_LIBRARY_PATH
ENV QT_QPA_PLATFORM=offscreen

# Persist env variables to root shell
RUN echo 'export PATH=/opt/Qt/6.11.1/gcc_64/bin:$PATH' >> /root/.bashrc && \
    echo 'export CMAKE_PREFIX_PATH=/opt/Qt/6.11.1/gcc_64/lib/cmake' >> /root/.bashrc && \
    echo 'export VCPKG_ROOT=/opt/vcpkg' >> /root/.bashrc && \
    echo 'export VCPKG_OVERLAY_PORTS=/root/LOUPE/vcpkg/overlays' >> /root/.bashrc && \
    echo 'export LOUPE_QT_ROOT=/opt/Qt/6.11.1/gcc_64' >> /root/.bashrc && \
    echo 'export LD_LIBRARY_PATH=/opt/Qt/6.11.1/gcc_64/lib:$LD_LIBRARY_PATH' >> /root/.bashrc && \
    echo 'export QT_QPA_PLATFORM=offscreen' >> /root/.bashrc

# Reinstall essential tools (redundant but safe)
RUN apt update && apt install -y build-essential cmake g++ make ninja-build && apt clean

# Final working directory
WORKDIR /root/LOUPE

RUN cmake -B build -S . -DLOUPE_INSTALL_QT_DEPENDENCIES=0 \
          -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_VCPKG_BUILD_TYPE=Release \
          -DVCPKG_OVERLAY_PORTS=vcpkg/overlays \
          -DLOUPE_INSTALL_TO_USR=OFF \
          -DCMAKE_INSTALL_PREFIX="/" \
          -DLOUPE_QT_ROOT=/opt/Qt/6.11.1/gcc_64
RUN cmake --build build --target all release_translations -j$(nproc)
RUN cmake --install build --prefix /install

# Delete all static libraries (they are not needed in the final docker image)
RUN find /opt/Qt/6.11.1/gcc_64/lib -type f -name "*.a" -delete

# Final working directory
WORKDIR /root

FROM ubuntu:22.04 AS runtime_loupe

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y \
    libglx-mesa0 \
    libgl1-mesa-glx \
    libglu1-mesa \
    libegl1-mesa \
    libx11-6 \
    libxext6 \
    libxrender1 \
    libxcb1 \
    libxcb-glx0 \
    libxcb-render0 \
    libxcb-shm0 \
    libpng16-16 \
    libfontconfig1 \
    libglib2.0-0 \
    libxkbcommon0 \
    libdbus-1-3 \
    && apt clean

COPY --from=builder_loupe /opt/Qt /opt/Qt
COPY --from=builder_loupe /install /

# Persist env variables to root shell
RUN echo 'export PATH=/opt/Qt/6.11.1/gcc_64/bin:$PATH' >> /root/.bashrc && \
    echo 'export LD_LIBRARY_PATH=/opt/Qt/6.11.1/gcc_64/lib:$LD_LIBRARY_PATH' >> /root/.bashrc && \
    echo 'export QT_QPA_PLATFORM=offscreen' >> /root/.bashrc

WORKDIR /root

CMD ["bash"]
