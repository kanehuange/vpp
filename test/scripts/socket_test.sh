#! /bin/bash
#
# socket_test.sh -- script to run socket tests.
#
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
vpp_dir="$WS_ROOT/build-root/install-vpp-native/vpp/bin/"
vpp_debug_dir="$WS_ROOT/build-root/install-vpp_debug-native/vpp/bin/"
vpp_shm_dir="/dev/shm/"
lib64_dir="$WS_ROOT/build-root/install-vpp-native/vpp/lib64/"
lib64_debug_dir="$WS_ROOT/build-root/install-vpp_debug-native/vpp/lib64/"
docker_vpp_dir="/vpp/"
docker_lib64_dir="/vpp-lib64/"
docker_os="ubuntu"
preload_lib="libvppsocketwrapper.so.0.0.0"
vpp_app="vpp"
sock_srvr_app="sock_test_server"
sock_clnt_app="sock_test_client"
sock_srvr_addr="127.0.0.1"
sock_srvr_port="22000"
iperf_srvr_app="iperf3 -V4d1 -s"
iperf_clnt_app="iperf3 -V4d -c localhost"
gdb_in_emacs="gdb_in_emacs"
vppcom_conf="vppcom.conf"
vppcom_conf_dir="$WS_ROOT/src/uri/"
docker_vppcom_conf_dir="/etc/vpp/"
xterm_geom="60x40"
bash_header="#! /bin/bash"
tmp_cmdfile_prefix="/tmp/socket_test_cmd"
cmd1_file="${tmp_cmdfile_prefix}1.$$"
cmd2_file="${tmp_cmdfile_prefix}2.$$"
cmd3_file="${tmp_cmdfile_prefix}3.$$"
tmp_gdb_cmdfile_prefix="/tmp/gdb_cmdfile"
def_gdb_cmdfile_prefix="$WS_ROOT/extras/gdb/gdb_cmdfile"
tmp_gdb_cmdfile_vpp="${tmp_gdb_cmdfile_prefix}_vpp.$$"
tmp_gdb_cmdfile_client="${tmp_gdb_cmdfile_prefix}_vcl_client.$$"
tmp_gdb_cmdfile_server="${tmp_gdb_cmdfile_prefix}_vcl_server.$$"
get_docker_server_ip4addr='srvr_addr=$(docker network inspect bridge | grep IPv4Address | awk -e '\''{print $2}'\'' | sed -e '\''s,/16,,'\'' -e '\''s,",,g'\'' -e '\''s/,//'\'')'
#' single quote to fix the confused emacs colorizer.
trap_signals="SIGINT SIGTERM EXIT"

# Set default values for imported environment variables if they don't exist.
#
VPP_GDB_CMDFILE="${VPP_GDB_CMDFILE:-${def_gdb_cmdfile_prefix}.vpp}"
VPPCOM_CLIENT_GDB_CMDFILE="${VPPCOM_CLIENT_GDB_CMDFILE:-${def_gdb_cmdfile_prefix}.vppcom_client}"
VPPCOM_SERVER_GDB_CMDFILE="${VPPCOM_SERVER_GDB_CMDFILE:-${def_gdb_cmdfile_prefix}.vppcom_server}"

usage() {
    cat <<EOF
Usage: socket_test.sh OPTIONS TEST
TESTS:
  nk, native-kernel   Run server & client on host using kernel.
  nv, native-vcl      Run vpp, server & client on host using VppComLib.
  np, native-preload  Run vpp, server & client on host using LD_PRELOAD.
  dk, docker-kernel   Run server & client in docker using kernel stack.
  dv, docker-vcl      Run vpp on host, server & client in docker using VppComLib.
  dp, docker-preload  Run vpp on host, server & client in docker using LD_PRELOAD.

OPTIONS:
  -h                  Print this usage text.
  -l                  Leave ${tmp_cmdfile_prefix}* files after test run.
  -b                  Run bash after application exit.
  -d                  Run the vpp_debug version of all apps.
  -c                  Set VPPCOM_CONF to use the vppcom_test.conf file.
  -i                  Run iperf3 for client/server app in native tests.
  -e a[ll]            Run all in emacs+gdb.
     c[lient]         Run client in emacs+gdb.
     s[erver]         Run server in emacs+gdb.
     v[pp]            Run vpp in emacs+gdb.
  -g a[ll]            Run all in gdb.
     c[lient]         Run client in gdb.
     s[erver]         Run server in gdb.
     v[pp]            Run vpp in gdb.
  -t                  Use tabs in one xterm if available (e.g. xfce4-terminal).

OPTIONS passed to client/server:
  -S <ip address>     Server IP address.
  -P <server port>    Server Port number.
  -E <data>           Run Echo test.
  -N <num-writes>     Test Cfg: number of writes.
  -R <rxbuf-size>     Test Cfg: rx buffer size.
  -T <txbuf-size>     Test Cfg: tx buffer size.
  -U                  Run Uni-directional test.
  -B                  Run Bi-directional test.
  -I <num-tst-socks>  Send data over multiple test sockets in parallel.
  -V                  Test Cfg: Verbose mode.
  -X                  Exit client/server after running test.

Environment variables:
  VPPCOM_CONF                Pathname of vppcom configuration file.
  VPP_GDB_CMDFILE            Pathname of gdb command file for vpp.
  VPPCOM_CLIENT_GDB_CMDFILE  Pathname of gdb command file for client.
  VPPCOM_SERVER_GDB_CMDFILE  Pathname of gdb command file for server.
EOF
    exit 1
}

declare -i emacs_vpp=0
declare -i emacs_client=0
declare -i emacs_server=0
declare -i gdb_vpp=0
declare -i gdb_client=0
declare -i gdb_server=0
declare -i perf_vpp=0
declare -i perf_client=0
declare -i perf_server=0
declare -i leave_tmp_files=0
declare -i bash_after_exit=0
declare -i iperf3=0

while getopts ":hitlbcde:g:p:E:I:N:P:R:S:T:UBVX" opt; do
    case $opt in
        h) usage ;;
        l) leave_tmp_files=1
           ;;
        b) bash_after_exit=1
           ;;
        i) iperf3=1
           ;;
        t) xterm_geom="180x40"
           use_tabs="true"
           ;;
        c) VPPCOM_CONF="${vppcom_conf_dir}vppcom_test.conf"
           ;;
        d) title_dbg="-DEBUG"
           vpp_dir=$vpp_debug_dir
           lib64_dir=$lib64_debug_dir
           ;;
        e) if [ $OPTARG = "a" ] || [ $OPTARG = "all" ] ; then
               emacs_client=1
               emacs_server=1
               emacs_vpp=1
           elif [ $OPTARG = "c" ] || [ $OPTARG = "client" ] ; then
               emacs_client=1
           elif [ $OPTARG = "s" ] || [ $OPTARG = "server" ] ; then
               emacs_server=1
           elif [ $OPTARG = "v" ] || [ $OPTARG = "vpp" ] ; then
               emacs_vpp=1
           else
               echo "ERROR: Option -e unknown argument \'$OPTARG\'" >&2
               usage
           fi
           title_dbg="-DEBUG"
           vpp_dir=$vpp_debug_dir
           lib64_dir=$lib64_debug_dir
           ;;
        g) if [ $OPTARG = "a" ] || [ $OPTARG = "all" ] ; then
               gdb_client=1
               gdb_server=1
               gdb_vpp=1
           elif [ $OPTARG = "c" ] || [ $OPTARG = "client" ] ; then
               gdb_client=1
           elif [ $OPTARG = "s" ] || [ $OPTARG = "server" ] ; then
               gdb_server=1
           elif [ $OPTARG = "v" ] || [ $OPTARG = "vpp" ] ; then
               gdb_vpp=1
           else
               echo "ERROR: Option -g unknown argument \'$OPTARG\'" >&2
               usage
           fi
           title_dbg="-DEBUG"
           vpp_dir=$vpp_debug_dir
           lib64_dir=$lib64_debug_dir
           ;;
        p) if [ $OPTARG = "a" ] || [ $OPTARG = "all" ] ; then
               perf_client=1
               perf_server=1
               perf_vpp=1
           elif [ $OPTARG = "c" ] || [ $OPTARG = "client" ] ; then
               perf_client=1
           elif [ $OPTARG = "s" ] || [ $OPTARG = "server" ] ; then
               perf_server=1
           elif [ $OPTARG = "v" ] || [ $OPTARG = "vpp" ] ; then
               perf_vpp=1
           else
               echo "ERROR: Option -p unknown argument \'$OPTARG\'" >&2
               usage
           fi
           echo "WARNING: -p options TBD"
           ;;
        S) sock_srvr_addr="$OPTARG"
           ;;
        P) sock_srvr_port="$OPTARG"
           ;;
E|I|N|R|T) sock_clnt_options="$sock_clnt_options -$opt \"$OPTARG\""
           ;;
  U|B|V|X) sock_clnt_options="$sock_clnt_options -$opt"
           ;;
       \?)
           echo "ERROR: Invalid option: -$OPTARG" >&2
           usage
           ;;
        :)
           echo "ERROR: Option -$OPTARG requires an argument." >&2
           usage
           ;;
    esac
done

shift $(( $OPTIND-1 ))
while ! [[ $run_test ]] && (( $# > 0 )) ; do
    case $1 in
        "nk" | "native-kernel")
            run_test="native_kernel" ;;
        "np" | "native-preload")
            run_test="native_preload" ;;
        "nv" | "native-vcl")
            sock_srvr_app="vcl_test_server"
            sock_clnt_app="vcl_test_client"
            run_test="native_vcl" ;;
        "dk" | "docker-kernel")
            run_test="docker_kernel" ;;
        "dp" | "docker-preload")
            run_test="docker_preload" ;;
        "dv" | "docker-vcl")
            sock_srvr_app="vcl_test_server"
            sock_clnt_app="vcl_test_client"
            run_test="docker_vcl" ;;
        *)
            echo "ERROR: Unknown option '$1'!" >&2
            usage ;;
    esac
    shift
done

if [ -z "$WS_ROOT" ] ; then
    echo "ERROR: WS_ROOT environment variable not set!" >&2
    echo "       Please set WS_ROOT to VPP workspace root directory." >&2
    env_test_failed="true"
fi

if [ ! -d $vpp_dir ] ; then
    echo "ERROR: Missing VPP$DEBUG bin directory!" >&2
    echo "       $vpp_dir" >&2
    env_test_failed="true"
fi

if [[ $run_test =~ .*"_preload" ]] ; then
   if [ ! -d $lib64_dir ] ; then
       echo "ERROR: Missing VPP$DEBUG lib64 directory!" >&2
       echo "       $lib64_dir" >&2
       env_test_failed="true"
   elif [ ! -f $lib64_dir$preload_lib ] ; then
       echo "ERROR: Missing VPP$DEBUG PRE_LOAD library!" >&2
       echo "       $lib64_dir$preload_lib" >&2
       env_test_failed="true"
   fi
fi

if [ ! -f $vpp_dir$vpp_app ] ; then
    echo "ERROR: Missing VPP$DEBUG Application!" >&2
    echo "       $vpp_dir$vpp_app" >&2
    env_test_failed="true"
fi

if [ ! -f $vpp_dir$sock_srvr_app ] ; then
    echo "ERROR: Missing$DEBUG Socket Server Application!" >&2
    echo "       $vpp_dir$sock_srvr_app" >&2
    env_test_failed="true"
fi

if [ ! -f $vpp_dir$sock_clnt_app ] ; then
    echo "ERROR: Missing$DEBUG Socket Client Application!" >&2
    echo "       $vpp_dir$sock_clnt_app" >&2
    env_test_failed="true"
fi

if [[ $run_test =~ "docker_".* ]] ; then
    if [ $emacs_client -eq 1 ] || [ $emacs_server -eq 1 ] || [ $gdb_client -eq 1 ] || [ $gdb_server -eq 1 ] ; then
        
        echo "WARNING: gdb is not currently supported in docker."
        echo "         Ignoring client/server gdb options."
        emacs_client=0
        emacs_server=0
        gdb_client=0
        gdb_server=0
    fi
fi

if [ -n "$env_test_failed" ] ; then
    exit 1
fi

if [ -f "$VPPCOM_CONF" ] ; then
    vppcom_conf="$(basename $VPPCOM_CONF)"
    vppcom_conf_dir="$(dirname $VPPCOM_CONF)/"
    api_prefix="$(egrep -s '^\s*api-prefix \w+' $VPPCOM_CONF | awk -e '{print $2}')"
    if [ -n "$api_prefix" ] ; then
        api_segment=" api-segment { prefix $api_prefix }"
    fi
fi
vpp_args="unix { interactive cli-listen /run/vpp/cli.sock }${api_segment}"

if [ $iperf3 -eq 1 ] &&  [[ ! $run_test =~ "docker_".* ]] ; then
    app_dir="$(dirname $(which iperf3))/"
    srvr_app=$iperf_srvr_app
    clnt_app=$iperf_clnt_app
else
    app_dir="$vpp_dir"
    srvr_app="$sock_srvr_app $sock_srvr_port"
    clnt_app="$sock_clnt_app${sock_clnt_options} \$srvr_addr $sock_srvr_port"
fi

verify_no_vpp() {
    local running_vpp="ps -eaf|grep -v grep|grep \"bin/vpp\""
    if [ "$(eval $running_vpp)" != "" ] ; then
        echo "ERROR: Please kill all running vpp instances:"
        echo
        eval $running_vpp
        echo
        exit 1
    fi
    clean_devshm="$vpp_shm_dir*db $vpp_shm_dir*global_vm $vpp_shm_dir*vpe-api $vpp_shm_dir[0-9]*-[0-9]* $vpp_shm_dir*:segment[0-9]*"
    rm -f $clean_devshm
    devshm_files="$(ls -l $clean_devshm 2>/dev/null | grep $(whoami))"
    if [ "$devshm_files" != "" ] ; then
        echo "ERROR: Please remove the following $vpp_shm_dir files:"
        for file in "$devshm_files" ; do
            echo "  $file"
        done
        exit 1
    fi
}

verify_no_docker_containers() {
    if (( $(which docker | wc -l) < 1 )) ; then
        echo "ERROR: docker is not installed!"
        echo "See https://docs.docker.com/engine/installation/linux/ubuntu/"
        echo " or https://docs.docker.com/engine/installation/linux/centos/"
        exit 1
    fi
    if (( $(docker ps | wc -l) > 1 )) ; then
        echo "ERROR: Run the following to kill all docker containers:"
        echo "docker kill \$(docker ps -q)"
        echo
        docker ps
        exit 1
    fi
}

set_pre_cmd() {
    # arguments
    #   $1 : emacs flag
    #   $2 : gdb flag
    #   $3 : optional LD_PRELOAD library pathname
    local -i emacs=$1
    local -i gdb=$2

    if [ $emacs -eq 1 ] ; then
        write_gdb_cmdfile $tmp_gdb_cmdfile $gdb_cmdfile $emacs $3
        pre_cmd="$gdb_in_emacs "
    elif [ $gdb -eq 1 ] ; then
        write_gdb_cmdfile $tmp_gdb_cmdfile $gdb_cmdfile $emacs $3
        pre_cmd="gdb -x $tmp_gdb_cmdfile -i=mi --args "
    elif [ -z $3 ] ; then
        unset -v pre_cmd
    else
        docker_ld_preload="-e LD_PRELOAD=$3 "
        pre_cmd="LD_PRELOAD=$3 "
    fi
}

write_script_header() {
    # arguments
    #   $1 : command script file
    #   $2 : gdb command file
    #   $3 : title
    #   $4 : optional command string (typically "sleep 2")
    echo "$bash_header" > $1
    echo -e "#\n# $1 generated on $(date)\n#" >> $1
    if [ $leave_tmp_files -eq 0 ] ; then
        echo "trap \"rm -f $1 $2\" $trap_signals" >> $1
    fi
    echo "export VPPCOM_CONF=${vppcom_conf_dir}${vppcom_conf}" >> $1
    if [ "$pre_cmd" = "$gdb_in_emacs " ] ; then
        cat <<EOF >> $1
$gdb_in_emacs() {
    emacs --eval "(gdb \"gdb -x $2 -i=mi --args \$*\")" --eval "(setq frame-title-format \"$3\")"
}
EOF
    fi
    if [ -n "$4" ] ; then
        echo "$4" >> $1
    fi
}

write_script_footer() {
    # arguments
    #   $1 : command script file
    #   $2 : perf flag indicating to run bash before exit
    local -i perf=$2
    if [ $bash_after_exit -eq 1 ] || [ $perf -eq 1 ] ; then
        echo "bash" >> $1
    fi
}

write_gdb_cmdfile() {
    # arguments
    #   $1 : gdb command file
    #   $2 : User specified gdb cmdfile
    #   $3 : emacs flag
    #   $4 : optional LD_PRELOAD library pathname.
    local -i emacs=$3
    
    echo "# $1 generated on $(date)" > $1
    echo "#" >> $1
    echo "set confirm off" >> $1
    if [ -n "$4" ] ; then
        echo "set exec-wrapper env LD_PRELOAD=$4" >> $1
        echo "start" >> $1
    fi

    if [ ! -f $2 ] ; then
        echo -n "# " >> $1
    fi
    echo "source $2" >> $1
    if [ $emacs -eq 0 ] ; then
        echo "run" >> $1
    fi
}

native_kernel() {
    banner="Running NATIVE-KERNEL socket test"
    
    title1="SERVER$title_dbg (Native-Kernel Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "${pre_cmd}${app_dir}${srvr_app}" >> $cmd1_file
    write_script_footer $cmd1_file $perf_server
    
    title2="CLIENT$title_dbg (Native-Kernel Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "srvr_addr=\"$sock_srvr_addr\"" >> $cmd2_file
    echo "${pre_cmd}${app_dir}${clnt_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_client

    chmod +x $cmd1_file $cmd2_file
}

native_preload() {
    verify_no_vpp
    banner="Running NATIVE-PRELOAD socket test"
    ld_preload="$lib64_dir$preload_lib "

    title1="VPP$title_dbg (Native-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_vpp
    gdb_cmdfile=$VPP_GDB_CMDFILE
    set_pre_cmd $emacs_vpp $gdb_vpp
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "${pre_cmd}$vpp_dir$vpp_app $vpp_args " >> $cmd1_file
    write_script_footer $cmd1_file $perf_vpp

    title2="SERVER$title_dbg (Native-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server $ld_preload
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "${pre_cmd}${app_dir}${srvr_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_server

    title3="CLIENT$title_dbg (Native-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client $ld_preload
    write_script_header $cmd3_file $tmp_gdb_cmdfile "$title3" "sleep 3"
    echo "srvr_addr=\"$sock_srvr_addr\"" >> $cmd3_file
    echo "${pre_cmd}${app_dir}${clnt_app}" >> $cmd3_file
    write_script_footer $cmd3_file $perf_client
    
    chmod +x $cmd1_file $cmd2_file $cmd3_file
}

native_vcl() {
    verify_no_vpp
    banner="Running NATIVE-VCL socket test"

    title1="VPP$title_dbg (Native-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_vpp
    gdb_cmdfile=$VPP_GDB_CMDFILE
    set_pre_cmd $emacs_vpp $gdb_vpp
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "${pre_cmd}$vpp_dir$vpp_app $vpp_args " >> $cmd1_file
    write_script_footer $cmd1_file $perf_vpp

    title2="SERVER$title_dbg (Native-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "export LD_LIBRARY_PATH=\"$lib64_dir:$LD_LIBRARY_PATH\"" >> $cmd2_file
    echo "${pre_cmd}${app_dir}${srvr_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_server

    title3="CLIENT$title_dbg (Native-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client
    write_script_header $cmd3_file $tmp_gdb_cmdfile "$title3" "sleep 3"
    echo "export LD_LIBRARY_PATH=\"$lib64_dir:$LD_LIBRARY_PATH\"" >> $cmd3_file
    echo "srvr_addr=\"$sock_srvr_addr\"" >> $cmd3_file
    echo "${pre_cmd}${app_dir}${clnt_app}" >> $cmd3_file
    write_script_footer $cmd3_file $perf_client
    
    chmod +x $cmd1_file $cmd2_file $cmd3_file
}

docker_kernel() {
    verify_no_docker_containers
    banner="Running DOCKER-KERNEL socket test"
    
    title1="SERVER$title_dbg (Docker-Native Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "docker run -it -v $vpp_dir:$docker_vpp_dir -p $sock_srvr_port:$sock_srvr_port $docker_os ${docker_vpp_dir}${srvr_app}" >> $cmd1_file
    write_script_footer $cmd1_file $perf_server
    
    title2="CLIENT$title_dbg (Docker-Native Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "$get_docker_server_ip4addr" >> $cmd2_file
    echo "docker run -it -v $vpp_dir:$docker_vpp_dir $docker_os ${docker_vpp_dir}${clnt_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_client
    
    chmod +x $cmd1_file $cmd2_file
}

docker_preload() {
    verify_no_vpp
    verify_no_docker_containers
    banner="Running DOCKER-PRELOAD socket test"
    ld_preload="$docker_lib64_dir$preload_lib "
    
    title1="VPP$title_dbg (Docker-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_vpp
    gdb_cmdfile=$VPP_GDB_CMDFILE
    set_pre_cmd $emacs_vpp $gdb_vpp
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "${pre_cmd}$vpp_dir$vpp_app $vpp_args" >> $cmd1_file
    write_script_footer $cmd1_file $perf_vpp
    
    title2="SERVER$title_dbg (Docker-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server $ld_preload
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "docker run -it -v $vpp_shm_dir:$vpp_shm_dir -v $vpp_dir:$docker_vpp_dir -v $lib64_dir:$docker_lib64_dir -v $vppcom_conf_dir:$docker_vppcom_conf_dir -p $sock_srvr_port:$sock_srvr_port -e VPPCOM_CONF=${docker_vppcom_conf_dir}/$vppcom_conf -e LD_LIBRARY_PATH=$docker_lib64_dir ${docker_ld_preload}$docker_os ${docker_vpp_dir}${srvr_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_server

    title3="CLIENT$title_dbg (Docker-Preload Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client $ld_preload
    write_script_header $cmd3_file $tmp_gdb_cmdfile "$title3" "sleep 3"
    echo "$get_docker_server_ip4addr" >> $cmd3_file
    echo "docker run -it -v $vpp_shm_dir:$vpp_shm_dir -v $vpp_dir:$docker_vpp_dir -v $lib64_dir:$docker_lib64_dir -v $vppcom_conf_dir:$docker_vppcom_conf_dir -e VPPCOM_CONF=${docker_vppcom_conf_dir}/$vppcom_conf -e LD_LIBRARY_PATH=$docker_lib64_dir ${docker_ld_preload}$docker_os ${docker_vpp_dir}${clnt_app}" >> $cmd3_file
    write_script_footer $cmd3_file $perf_client

    chmod +x $cmd1_file $cmd2_file $cmd3_file
}

docker_vcl() {
    verify_no_vpp
    verify_no_docker_containers
    banner="Running DOCKER-VCL socket test"
    
    title1="VPP$title_dbg (Docker-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_vpp
    gdb_cmdfile=$VPP_GDB_CMDFILE
    set_pre_cmd $emacs_vpp $gdb_vpp
    write_script_header $cmd1_file $tmp_gdb_cmdfile "$title1"
    echo "${pre_cmd}$vpp_dir$vpp_app $vpp_args" >> $cmd1_file
    write_script_footer $cmd1_file $perf_vpp
    
    title2="SERVER$title_dbg (Docker-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_server
    gdb_cmdfile=$VPPCOM_SERVER_GDB_CMDFILE
    set_pre_cmd $emacs_server $gdb_server
    write_script_header $cmd2_file $tmp_gdb_cmdfile "$title2" "sleep 2"
    echo "docker run -it -v $vpp_shm_dir:$vpp_shm_dir -v $vpp_dir:$docker_vpp_dir -v $lib64_dir:$docker_lib64_dir -v $vppcom_conf_dir:$docker_vppcom_conf_dir -p $sock_srvr_port:$sock_srvr_port -e VPPCOM_CONF=${docker_vppcom_conf_dir}/$vppcom_conf -e LD_LIBRARY_PATH=$docker_lib64_dir $docker_os ${docker_vpp_dir}${srvr_app}" >> $cmd2_file
    write_script_footer $cmd2_file $perf_server

    title3="CLIENT$title_dbg (Docker-VCL Socket Test)"
    tmp_gdb_cmdfile=$tmp_gdb_cmdfile_client
    gdb_cmdfile=$VPPCOM_CLIENT_GDB_CMDFILE
    set_pre_cmd $emacs_client $gdb_client
    write_script_header $cmd3_file $tmp_gdb_cmdfile "$title3" "sleep 3"
    echo "$get_docker_server_ip4addr" >> $cmd3_file
    echo "docker run -it -v $vpp_shm_dir:$vpp_shm_dir -v $vpp_dir:$docker_vpp_dir -v $lib64_dir:$docker_lib64_dir -v $vppcom_conf_dir:$docker_vppcom_conf_dir -e VPPCOM_CONF=${docker_vppcom_conf_dir}/$vppcom_conf -e LD_LIBRARY_PATH=$docker_lib64_dir $docker_os ${docker_vpp_dir}${clnt_app}" >> $cmd3_file
    write_script_footer $cmd3_file $perf_client

    chmod +x $cmd1_file $cmd2_file $cmd3_file
}

if [[ $run_test ]] ; then
    eval $run_test
else
    echo "ERROR: Please specify a test to run!" >&2
    usage;
fi

if (( $(which xfce4-terminal | wc -l) > 0 )) ; then
    xterm_cmd="xfce4-terminal --geometry $xterm_geom"
    if [[ $use_tabs ]] ; then
        if [ -x "$cmd3_file" ] ; then
            $xterm_cmd  --title "$title1" --command "$cmd1_file" --tab --title "$title2" --command "$cmd2_file" --tab --title "$title3" --command "$cmd3_file"
        else
            $xterm_cmd --title "$title1" --command "$cmd1_file" --tab --title "$title2" --command "$cmd2_file"
        fi
    else
        ($xterm_cmd --title "$title1" --command "$cmd1_file" &)
        ($xterm_cmd --title "$title2" --command "$cmd2_file" &)
        if [ -x "$cmd3_file" ] ; then
            ($xterm_cmd --title "$title3" --command "$cmd3_file" &)
        fi
    fi
        
else
    if [[ $use_tabs ]] ; then
        echo "Sorry, plain ol' xterm doesn't support tabs."
    fi
    xterm_cmd="xterm -fs 10 -geometry $xterm_geom"
    ($xterm_cmd -title "$title1" -e "$cmd1_file" &)
    ($xterm_cmd -title "$title2" -e "$cmd2_file" &)
    if [ -x "$cmd3_file" ] ; then
        ($xterm_cmd -title "$title3" -e "$cmd3_file" &)
    fi
fi

sleep 1
