FROM carla-prerequisites:latest

USER carla
WORKDIR /home/carla

RUN cd /home/carla/ && \
  git clone --depth 1 --branch 0.9.13 https://github.com/faizansid77/carla.git && \
  cd /home/carla/carla && \
  ./Update.sh && \
  make CarlaUE4Editor && \
  make PythonAPI && \
  make build.utils && \
  make package && \
  rm -r /home/carla/carla/Dist

WORKDIR /home/carla/carla
