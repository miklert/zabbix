<?php

/*
 * A helper class for working with ClickHouse database
 */
class CClickHouseHelper {

	/**
	 * Perform request(s) to Elasticsearch and parse the results.
	 *
	 * @param string $method      HTTP method to be used to perform request
	 * @param string $endpoint    requested url
	 * @param mixed  $request     data to be sent
	 *
	 * @return array    parsed result
	 */
	public static function query($request,$is_table_result,$columns) {
		
		global $HISTORY;
//		error("CClikHouseHelper.query($request) ");
		$ch = curl_init();

		curl_setopt($ch, CURLOPT_URL,$HISTORY['url']['uint']);
		curl_setopt($ch, CURLOPT_POST, 1);
		curl_setopt($ch, CURLOPT_POSTFIELDS,$request);

		// receive server response ...
		curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);

		$server_output = curl_exec ($ch);
		curl_close ($ch);

//		error("Clickhouse returned '$server_output'");

//		ob_start();
//    		var_dump($request);
//    		var_dump($server_output);

//		$dresult = ob_get_clean();
//	    error("Dump of the result is '$dresult'");

//		file_put_contents('/var/log/nginx/clickhouse.log', "Clickhouse Results structure is $dresult' \n",FILE_APPEND);



		return self::parseResult($server_output,$is_table_result,$columns);
		
	}

	/**
	 * Parse result and return two dimentional array of the result
	 *
	 * @param string $data        result as a string
	 *
	 * @return array    parsed result  two dimentional array of the result
	 */
	private static function parseResult($data,$is_table_result,$columns) {

	    //to make processing simpler, lets distinguish results of two types - table (two dimensional array as result
	    //returned or SINGLE, when one value (a number, probably) 
	    if ($is_table_result) 
	    {
		$result=[];

		$lines = explode("\n", $data);
		$curline=0;

		foreach ($lines as $line) {
		    if (strlen(str_replace("\n",'',$line) ) > 0) 
		    { 
//			error("Processing line '$line'");
//			error("Columns count is ".count($columns)." field count is ".count(explode("\t",$line)));
			$result[$curline]=array_combine($columns,explode("\t",$line));
			$curline++;

		    } else {
//			error("Got empty line, skipping");
		    }
		}

	    } else
	    {
		//single result is here, stripping tabs,spaces and newlines
		$result=str_replace(array("\r", "\n","\t"), '', $data);
	    }

/*	    ob_start();
	    var_dump($result);
	    $dresult = ob_get_clean();
	    error("Dump of the result is '$dresult'");
*/
	    return $result;
	}


}



