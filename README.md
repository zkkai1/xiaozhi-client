1.该版本在原有基础上增加了自定义唤醒词的功能，使用乐鑫官方的multinet模型监听关键词，监听到后Application::GetInstance().WakeWordInvoke(xiaoai)

2.自定义唤醒词时，需改动menuconfig-> ESP Speech Recognition->Add Chinese speech commands(拼音拼写，每个字空格隔开，不能有特殊符号；并将Chinese Speech Commands Model选为mn5q8_cn)

wake_word_detect.cc中添加唤醒词，与menuconfig保持一致

application.cc中wake_word_detect_.OnWakeCommandDetected(命令词回调)中按需修改
