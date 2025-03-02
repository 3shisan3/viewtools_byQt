翻译文件生成手动操作，不写到cmake中，避免每次编译都更新生成

1.lupdate.sh用于创建和更新ts文件；
2.lrelease.sh用于将ts文件编译为qm文件;

example:
在项目根目录下执行
./script/translate/lupdate.sh /home/shisan/Qt/5.15.2/gcc_64/bin ./src ./ui/i18n
./script/translate/lrelease.sh /home/shisan/Qt/5.15.2/gcc_64/bin ./ui/i18n ./ui/i18n