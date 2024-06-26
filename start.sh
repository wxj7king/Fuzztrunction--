#!/usr/bin/env bash

# https://github.com/fuzztruction/fuzztruction/blob/main/env/start.sh
set -eu
set -o pipefail

text_red=$(tput setaf 1)
text_green=$(tput setaf 2)
text_bold=$(tput bold)
text_reset=$(tput sgr0)

function log_success {
    echo "${text_bold}${text_green}${1}${text_reset}"
}

container_name="ftmm"
image_name="${container_name}:latest"
container="$(docker ps --filter="name=$container_name" --latest --quiet)"
if [[ -n "$container" ]]; then
    # Connec to already running container
    log_success "[+] Found running instance: $container, connecting..."
    cmd="docker start $container"
    log_success "[+] $cmd"
    $cmd > /dev/null

    cmd="docker exec -it --workdir /home/user/FuzztructionMM $container zsh"
    log_success "[+] $cmd"
    $cmd
    exit 0
fi

log_success "[+] Creating new container..."

mounts=""
mounts+=" -v $PWD:/home/user/FuzztructionMM "
# you can expose directory inside the container for data exchange

cmd="docker run -ti -d --privileged
    $mounts
    --ulimit msgqueue=2097152000
    --net=host
    --name $container_name "

cmd+=" ${image_name}"
log_success "[+] $(echo $cmd | xargs)"
$cmd > /dev/null

log_success "[+] Rerun $0 to connect to the new container."
