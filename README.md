# zabbix

I have decided to release all the changes we've talked with Maxim on Zabbix Summit 2018 based on 4.0 LTS version.
Since the version has came out only a few days ago, I need some time to merge changes and to do a minimal testing.
I am going to release it by October, 15. 

Use the clickhouse patch to add clickhouse support, the patch must work on all V4 zabbix versions however i haven't check it's compatibility on different linux flavors. 

You will need curl libraries, use --with-curl option on compiling the server.

the following options are used to setup clickhouse in zabbix_server.conf:

########## History options
HistoryStorageURL=http://127.0.0.1:8123
HistoryStorageTypes=uint,dbl,str
HistoryStorageType=clickhouse
HistoryStorageTableName=zabbix.test_history_buffer

And almost similar for frontend - zabbix.conf.php

//Clickhouse configuration example //
global $HISTORY;
$HISTORY['storagetype']='clickhouse';

$HISTORY['url']   = [
		'uint' => 'http://127.0.0.1:8123',
		'dbl' => 'http://127.0.0.1:8123',
		'str' => 'http://127.0.0.1:8123'
];

$HISTORY['types'] = ['uint', 'dbl','str'];
$HISTORY['tablename'] = 'zabbix.test_history';


Please wait a bit to have optimized zabbix_server to be released.

However you can use testing development 3.4-based version https://github.com/miklert/xe-rabbix. 
Please note that this version is unstable and must be considered only for using it as a source for your own modifications or just to get the ideas to make similar changes.

If you consider using it, please use the version that was committed by the end of july. 
It will have some SNMP limitations (NO LLDP support) but have proved to be working in the production for several months.

