echo 'source ~/.bashrc
export PS1="\e[1;34m\u@\h \w> \e[m"' > /tmp/my_bash.rc
sudo ip netns exec dnode2 sudo -u stas bash --rcfile /tmp/my_bash.rc -i
