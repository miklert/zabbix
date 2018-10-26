# zabbix

So, I am finally releasing the changes I did to make zabbix somewhat faster or let me say, better.
Here the most important ones:

0. Before you start please make sure that the right reason to start using this code is your will to get some new experience or achieve an extraordinary results.
It’s very likely something will break, won’t work and you will be the only one to deal with it (however I will be glad to answer some questions you might have). If you need strong and reliable production system, get a clean vanilla version of zabbix, buy a support. 

## So, the short list of changes:
1. Clickhouse history offloading. Enjoy having data for years without MySQL/Postgress hassle at 50kNVPS.
2. Asynchronous SNMP processing. Beware! “Discovery” items will work the old slow synchronous way
3. Surprise… Asynchronous agent polling. Enjoy polling all your passive agents in a breeze. A couple of async agent polling threads will do all the work. Ok, ok, maybe 3 or 4 for really big installs (thousands of hosts)
4. And a Frankenstein – unreachable poller combines two worlds now – it will try async methods first and after failing them, will use sync methods/
5. Nmap accessibility checks. IPv4 only. Let me know if you need IPv6, and why.
6. Preproc manager with two sockets and queuing control. For those who monitors on really tight hardware.
7. Sorry guys, no “fast” widgets yet. They coming. A sort of.  I just need to rethink a few points. However for “problems.get” message is working on server. Feel free to use it, and please note that you’ll get only the problems happened since the server start.
8. Proxy is not tested yet. We don’t use them anymore. No reason. But sure this is coming also.
9. Worker locks are fixed by zabbix team, thank you guys.

## First of all, what version ? 

This is today’s zabbix-4.0.1rc2
The <Patch link> will work on both rc1 and rc2 and probably won’t on 4.0.0lts (there are nice cosmetic changes in main THREAD loops which make patch not compartible with 4.0.0). However, good news: there is no DB upgrade, so you can go back/forward without db backup and rollback (well, backup anyway, at least sometimes).

## Now, how to put it all together.
First, download the sources:
git clone 

or the patch.
Place patch (https://github.com/miklert/zabbix/blob/master/zabbix.patch) inside zabbix-4.0.Xxx folder and patch it
patch -p1  < the_patch

Then configure, setup, prepare the usual way: https://www.zabbix.com/documentation/4.0/manual/installation/install

And now, the new part: 
## 1. Set up pollers:

The two fellows are responsible for async SNMP and AGENT collection:

StartPollersAsyncSNMP=10
StartPollersAsyncAGENT=10

You don’t really need many of them. Typically they proccess 600-800 items a second:
./zabbix_server: async snmp poller #23 [got 8192 values in 13.841244 sec, getting values]

Feel free to switch them off by setting =0, so zabbix_server will poll the usual way, using sync processing otherwise they will handle all whatever traffic they can handle.

## 2. The Clickhouse setup.
I’ve wrote a post someday: https://mmakurov.blogspot.com/2018/07/zabbix-clickhouse-details.html

## 3. Nmap:
Zabbix server will use nmap for icmp* checks with packet count set to 1.
If you need granular packet loss, say 58%, calculate it in triggers. And setup such an accessibility checks each 10-15 seconds

Now, important note about delays: as items are processed in a bulk way (and also due to my laziness), they are coming back to queue altogether when all items has been polled. That takes 4-7 seconds in our setup. And a few seconds are needed to processing. So, don’t expect delays to be less then 10 seconds. 

However I would be interested to know if you do have such a requirements, perhaps, i’ll have a motivation to optimize it.

## 4. No new widgets
I really don’t want to release widgets yet as they are still in prototype stage and there is a big architecture problem – what they show is the only true since zabbix restart. I have two alternatives to fix that – either force zabbix_server on start to load all active problems from DB to memory or to ignore DB state on zabbix start and consider all triggers in OK state. Which will break problems start time.  And there is really simple fix possible, so I will add separate fixed widgets soon.

## 5. Proxy compiles, but I haven’t tested it at all. 
