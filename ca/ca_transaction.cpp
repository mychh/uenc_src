#include <stdio.h>
#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <algorithm>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <shared_mutex>
#include <mutex>
#include "ca_transaction.h"
#include "ca_message.h"
#include "ca_hexcode.h"
#include "ca_buffer.h"
#include "ca_serialize.h"
#include "ca_util.h"
#include "ca_global.h"
#include "ca_coredefs.h"
#include "ca_hexcode.h"
#include "Crypto_ECDSA.h"
#include "../include/cJSON.h"
#include "ca_interface.h"
#include "../include/logging.h"
#include "ca.h"
#include "ca_test.h"
#include "./proto/block.pb.h"
#ifndef _CA_FILTER_FUN_
#include "../include/net_interface.h"
#endif
#include "ca_clientinfo.h"
#include "../include/ScopeGuard.h"
#include "ca_clientinfo.h"
#include "../common/config.h"
#include "ca_clientinfo.h"
#include "ca_console.h"
#include "ca_device.h"
#include <string.h>
#include "ca_header.h"
#include "ca_sha2.h"
#include "ca_base64.h"
#include "ca_txhelper.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "../utils/time_util.h"

static std::mutex s_ResMutex;
#include "./ca_blockpool.h"
#include "../utils/string_util.h"
#include "getmac.pb.h"
#include "ca_AwardAlgorithm.h"

#include "proto/ca_protomsg.pb.h"
#include "ca_txhelper.h"
#include "ca_findsignnode.h"


std::shared_mutex MinutesCountLock;


int StringSplit(std::vector<std::string>& dst, const std::string& src, const std::string& separator)
{
    if (src.empty() || separator.empty())
        return 0;

    int nCount = 0;
    std::string temp;
    size_t pos = 0, offset = 0;

    
    while((pos = src.find_first_of(separator, offset)) != std::string::npos)
    {
        temp = src.substr(offset, pos - offset);
        if (temp.length() > 0){
            dst.push_back(temp);
            nCount ++;
        }
        offset = pos + 1;
    }

    
    temp = src.substr(offset, src.length() - offset);
    if (temp.length() > 0){
        dst.push_back(temp);
        nCount ++;
    }

    return nCount;
}

uint64_t CheckBalanceFromRocksDb(const std::string & address)
{
	if (address.size() == 0)
	{
		return 0;
	}

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	int64_t balance = 0;
	int r = pRocksDb->GetBalanceByAddress(txn, address, balance);
	if (r != 0)
	{
		return 0;
	}

	return balance;
}

bool FindUtxosFromRocksDb(const std::string & fromAddr, const std::string & toAddr, uint64_t amount, uint32_t needVerifyPreHashCount, uint64_t minerFees, CTransaction & outTx, std::string utxoStr)
{
	if (fromAddr.size() == 0 || toAddr.size() == 0)
	{
		error("FindUtxosFromRocksDb fromAddr toAddr ==0");
		return false;
	}
	
	uint64_t totalGasFee = (needVerifyPreHashCount - 1) * minerFees;
	uint64_t amt = fromAddr == toAddr ? totalGasFee : amount + totalGasFee;
	
    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		
		return false;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	uint64_t total = 0;
	std::vector<std::string> utxoHashs;
	std::vector<std::string> pledgeUtxoHashs;

	
	if (fromAddr == toAddr)
	{
		db_status = pRocksDb->GetPledgeAddressUtxo(txn, fromAddr, pledgeUtxoHashs);
		if (db_status != 0)
		{
			return false;
		}

		std::string strTxRaw;
		if (pRocksDb->GetTransactionByHash(txn, utxoStr, strTxRaw) != 0)
		{
			return false;
		}

		CTransaction utxoTx;
		utxoTx.ParseFromString(strTxRaw);

		for (int i = 0; i < utxoTx.vout_size(); i++)
		{
			CTxout txout = utxoTx.vout(i);
			if (txout.scriptpubkey() != VIRTUAL_ACCOUNT_PLEDGE)
			{
				continue;
			}
			amount = txout.value();
		}
	}
	
	db_status = pRocksDb->GetUtxoHashsByAddress(txn, fromAddr, utxoHashs);
	if (db_status != 0) {
		error("FindUtxosFromRocksDb GetUtxoHashsByAddress");
		return false;
	}
	
	
	std::set<std::string> setTmp(utxoHashs.begin(), utxoHashs.end());
	utxoHashs.clear();
	utxoHashs.assign(setTmp.begin(), setTmp.end());

	std::reverse(utxoHashs.begin(), utxoHashs.end());
	if (pledgeUtxoHashs.size() > 0)
	{
		std::reverse(pledgeUtxoHashs.begin(), pledgeUtxoHashs.end());
	}

	
	std::vector<std::string> vinUtxoHash;

	for (auto item : utxoHashs)
	{
		std::string strTxRaw;
		if (pRocksDb->GetTransactionByHash(txn, item, strTxRaw) != 0)
		{
			continue;
		}

		CTransaction utxoTx;
		utxoTx.ParseFromString(strTxRaw);

		for (int i = 0; i < utxoTx.vout_size(); i++)
		{
			CTxout txout = utxoTx.vout(i);
			if (txout.scriptpubkey() != fromAddr)
			{
				continue;
			}
			else
			{
				total += txout.value();

				CTxin * txin = outTx.add_vin();
				CTxprevout * prevout = txin->mutable_prevout();
				prevout->set_hash(utxoTx.hash());
				prevout->set_n(utxoTx.n());

				vinUtxoHash.push_back(utxoTx.hash());
			}

			
			if (i < utxoTx.vout_size() - 1)
			{
				continue;
			}

			if (total >= amt)
			{
				break;
			}
		}

		if (total >= amt)
		{
			break;
		}
	}


	if (total < amt)
	{
		error("FindUtxosFromRocksDb total < amt");
		return false;
	}

	if (fromAddr == toAddr)
	{
		std::string utxoTxStr;
		if (pRocksDb->GetTransactionByHash(txn, utxoStr, utxoTxStr) != 0)
		{
			error("GetTransactionByHash error!");
			return false;
		}

		CTransaction utxoTx;
		utxoTx.ParseFromString(utxoTxStr);
		for (int i = 0; i < utxoTx.vout_size(); i++)
		{
			CTxout txout = utxoTx.vout(i);
			if (txout.scriptpubkey() != VIRTUAL_ACCOUNT_PLEDGE)
			{
				bool isAlreadyAdd = false;
				for (auto hash : vinUtxoHash)
				{
					
					if (hash == utxoTx.hash())
					{
						isAlreadyAdd = true;
					}
				}

				
				if (txout.scriptpubkey() == fromAddr && !isAlreadyAdd)
				{
					for (auto utxo : utxoHashs)
					{
						
						if (utxo == utxoTx.hash())
						{
							total += txout.value();
						}
					}
				}
				continue;
			}

			CTxin * txin = outTx.add_vin();
			CTxprevout * prevout = txin->mutable_prevout();
			prevout->set_hash(utxoTx.hash());
			prevout->set_n(utxoTx.n());
		}
	}

	CTxout * txoutToAddr = outTx.add_vout();
	txoutToAddr->set_value(amount);
	txoutToAddr->set_scriptpubkey(toAddr);

	CTxout * txoutFromAddr = outTx.add_vout();
	txoutFromAddr->set_value(total - amt);
	txoutFromAddr->set_scriptpubkey(fromAddr);

	uint64_t time = Singleton<TimeUtil>::get_instance()->getTimestamp();
	outTx.set_time(time);
	outTx.set_txowner(fromAddr);
	outTx.set_ip(net_get_self_node_id());
	
	return true;
}

TransactionType CheckTransactionType(const CTransaction & tx)
{
	if( tx.time() == 0 || tx.hash().length() == 0 || tx.vin_size() == 0 || tx.vout_size() == 0)
	{
		return kTransactionType_Unknown;
	}

	CTxin txin = tx.vin(0);
	if ( txin.scriptsig().sign() == std::string(FEE_SIGN_STR))
	{
		return kTransactionType_Fee;
	}
	else if (txin.scriptsig().sign() == std::string(EXTRA_AWARD_SIGN_STR))
	{
		return kTransactionType_Award;
	}

	return kTransactionType_Tx;
}

bool checkTop(int top)
{
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();

	bool bRollback = true;
	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, bRollback);
	};

	unsigned int mytop = 0;
	pRocksDb->GetBlockTop(txn, mytop);	

	if(top < (int)mytop - 1 )
	{
		error("checkTop fail other top:%d my top:%d", top, (int)mytop);
		return false;
	}
	else if(top > (int)mytop + 1)
	{
		error("checkTop other top:%d my top:%d", top, (int)mytop);
		return false;
	}else
	{
		return true;
	}
}

bool checkTransaction(const CTransaction & tx)
{
	if (tx.vin_size() == 0 || tx.vout_size() == 0)
	{
		return false;
	}

	uint64_t total = 0;
	for (int i = 0; i < tx.vout_size(); i++)
	{
		CTxout txout = tx.vout(i);
		total += txout.value();
	}

	
	if (total < 0 || total > 21000000LL * COIN)
	{
		return false;
	}

	std::vector<CTxin> vTxins;
	for (int i = 0; i < tx.vin_size(); i++)
	{
		vTxins.push_back(tx.vin(i));
	}

	bool isRedeem = false;
	nlohmann::json extra = nlohmann::json::parse(tx.extra());
	std::string txType = extra["TransactionType"].get<std::string>();
	if (txType == TXTYPE_REDEEM)
	{
		isRedeem= true;
	}

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if ( txn == NULL )
	{
		error("(FindUtxosFromRocksDb) TransactionInit failed !");
		return -2;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, false);
	};

	int db_status = 0;

	
	std::sort(vTxins.begin(), vTxins.end(), [](const CTxin & txin0, const CTxin & txin1){
		if (txin0.prevout().n() > txin1.prevout().n())
		{
			return true;
		}
		else
		{
			return false;
		}
	});
	auto iter = std::unique(vTxins.begin(), vTxins.end(), [](const CTxin & txin0, const CTxin & txin1){
		return txin0.prevout().n() == txin1.prevout().n() &&
				txin0.prevout().hash() == txin1.prevout().hash() &&
				txin0.scriptsig().sign() == txin1.scriptsig().sign();
	});

	if (iter != vTxins.end())
	{
		if (isRedeem)
		{
			std::vector<std::string> utxos;
			string txowner = TxHelper::GetTxOwner(tx)[0];
			db_status = pRocksDb->GetPledgeAddressUtxo(txn, TxHelper::GetTxOwner(tx)[0], utxos);
			if (db_status != 0)
			{
				return false;
			}
			auto utxoIter = find(utxos.begin(), utxos.end(), iter->prevout().hash());
			if (utxoIter == utxos.end())
			{
				std::string txRaw;
				db_status = pRocksDb->GetTransactionByHash(txn, iter->prevout().hash(), txRaw);
				if (db_status != 0)
				{
					return false;
				}

				CTransaction utxoTx;
				utxoTx.ParseFromString(txRaw);
				if (utxoTx.vout_size() == 2)
				{
					if (utxoTx.vout(0).scriptpubkey() != utxoTx.vout(1).scriptpubkey())
					{
						return false;
					}
				}
			}
		}
		else
		{
			std::string txRaw;
			db_status = pRocksDb->GetTransactionByHash(txn, iter->prevout().hash(), txRaw);
			if (db_status != 0)
			{
				return false;
			}

			CTransaction utxoTx;
			utxoTx.ParseFromString(txRaw);
			if (utxoTx.vout_size() == 2)
			{
				if (utxoTx.vout(0).scriptpubkey() != utxoTx.vout(1).scriptpubkey())
				{
					return false;
				}
			}
		}
	}

	if (CheckTransactionType(tx) == kTransactionType_Tx)
	{
		
		for (auto txin : vTxins)
		{

			if (txin.prevout().n() == 0xFFFFFFFF)
			{
				return false;
			}
		}
	}
	else
	{
		
		unsigned int height = 0;
		db_status = pRocksDb->GetBlockTop(txn, height);
        if (db_status != 0) {
            return false;
        }
		if (tx.signprehash().size() > 0 && 0 == height)
		{
			return false;
		}

		CTxin txin0 = tx.vin(0);
		int scriptSigLen = txin0.scriptsig().sign().length() + txin0.scriptsig().pub().length();
		if (scriptSigLen < 2 || scriptSigLen > 100)
		{
			return false;
		}

		for (auto txin : vTxins)
		{
			if (height == 0 && (txin.scriptsig().sign() + txin.scriptsig().pub()) == OTHER_COIN_BASE_TX_SIGN)
			{
				return false;
			}
		}
	}
	
	return true;
}

std::vector<std::string> randomNode(unsigned int n)
{
	std::vector<std::string> v = net_get_node_ids();
	unsigned int nodeSize = n;
	std::vector<std::string> sendid;
	if ((unsigned int)v.size() < nodeSize)
	{
		debug("not enough node to send");
		return  sendid;
	}

	std::string s = net_get_self_node_id();
	auto iter = std::find(v.begin(), v.end(), s);
	if (iter != v.end())
	{
		v.erase(iter);
	}

	std::set<int> rSet;
	srand(time(NULL));
	while (1)
	{
	    int i = rand() % v.size();
	    rSet.insert(i);
	    if (rSet.size() == nodeSize)
	    {
			break;
	    }
	}

	for (auto i : rSet)
	{
	    sendid.push_back(v[i]);
	}

	return sendid;
}

int GetSignString(const std::string & message, std::string & signature, std::string & strPub)
{
	if (message.size() <= 0)
	{
		error("(GetSignString) parameter is empty!");
		return -1;
	}

	bool result = false;
	result = SignMessage(g_privateKey, message, signature);
	if (!result)
	{
		return -1;
	}

	GetPublicKey(g_publicKey, strPub);
	return 0;
}


int CreateTransactionFromRocksDb( const std::shared_ptr<CreateTxMsgReq>& msg, std::string &serTx)
{
	if ( msg == NULL )
	{
		return -1;
	}

	CTransaction outTx;
	double amount = stod( msg->amt() );
	uint64_t amountConvert = amount * DECIMAL_NUM;
	double minerFeeConvert = stod( msg->minerfees() );
	if(minerFeeConvert <= 0)
	{
		return -2;
	}
	uint64_t gasFee = minerFeeConvert * DECIMAL_NUM;

	uint32_t needVerifyPreHashCount = stoi( msg->needverifyprehashcount() );

    std::vector<std::string> fromAddr;
    fromAddr.emplace_back(msg->from());

    std::map<std::string, int64_t> toAddr;
    toAddr[msg->to()] = amountConvert;

	int ret = TxHelper::CreateTxMessage(fromAddr,toAddr, needVerifyPreHashCount, gasFee, outTx);
	if( ret != 0)
	{
		error("CreateTransaction Error ...\n");
		return -3;
	}
	
	for (int i = 0; i < outTx.vin_size(); i++)
	{
		CTxin * txin = outTx.mutable_vin(i);;
		txin->clear_scriptsig();
	}
	
	serTx = outTx.SerializeAsString();
	return 0;
}


int GetRedemUtxoAmount(const std::string & redeemUtxoStr, uint64_t & amount)
{
	if (redeemUtxoStr.size() != 64)
	{
		error("(GetRedemUtxoAmount) param error !");
		return -1;
	}

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		return -2;
	}

	bool bRollback = true;
	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, bRollback);
	};

	std::string txRaw;
	if ( 0 != pRocksDb->GetTransactionByHash(txn, redeemUtxoStr, txRaw) )
	{
		error("(GetRedemUtxoAmount) GetTransactionByHash failed !");
		return -3;
	}

	CTransaction tx;
	tx.ParseFromString(txRaw);

	for (int i = 0; i < tx.vout_size(); ++i)
	{
		CTxout txout = tx.vout(i);
		if (txout.scriptpubkey() == VIRTUAL_ACCOUNT_PLEDGE)
		{
			amount = txout.value();
		}
	}

	return 0;
}


bool VerifyBlockHeader(const CBlock & cblock)
{
	
	std::string hash = cblock.hash();
    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		return false;
	}

	bool bRollback = true;
	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, bRollback);
	};

	std::string strTempHeader;
	db_status = pRocksDb->GetBlockByBlockHash(txn, hash, strTempHeader);

	if (strTempHeader.size() != 0)
	{
		std::cout<<"BlockInfo has exist , do not need to add ..."<<std::endl;
		debug("BlockInfo has exist , do not need to add ...");
		bRollback = true;
        return false;
	}

	std::string strPrevHeader;
	db_status = pRocksDb->GetBlockByBlockHash(txn, cblock.prevhash(), strPrevHeader);
    if (db_status != 0) {
    }
	if (strPrevHeader.size() == 0)
	{
		error("bp_block_valid lookup hashPrevBlock ERROR !!!");
		bRollback = true;
		return false;
	}

	

	
	std::string strGenesisBlockHash;
	db_status = pRocksDb->GetBlockHashByBlockHeight(txn, 0, strGenesisBlockHash);
	if (db_status != 0)
	{
		error("GetBlockHashByBlockHeight failed!" );
		return false;
	}
	std::string strGenesisBlockHeader;
	pRocksDb->GetBlockByBlockHash(txn, strGenesisBlockHash, strGenesisBlockHeader);
	if (db_status != 0)
	{
		error("GetBlockHashByBlockHeight failed!" );
		return false;
	}

	if (strGenesisBlockHeader.length() == 0)
	{
		error("Genesis Block is not exist");
		return false;
	}
	
	CBlock genesisBlockHeader;
	genesisBlockHeader.ParseFromString(strGenesisBlockHeader);
	uint64_t blockHeaderTimestamp = cblock.time();
	uint64_t genesisBlockHeaderTimestamp = genesisBlockHeader.time();
	if (blockHeaderTimestamp == 0 || genesisBlockHeaderTimestamp == 0 || blockHeaderTimestamp <= genesisBlockHeaderTimestamp)
	{
		error("block timestamp error!");
		return false;
	}

	if (cblock.txs_size() == 0)
	{
		error("cblock.txs_size() == 0");
		bRollback = true;
		return false;
	}

	if (cblock.SerializeAsString().size() > MAX_BLOCK_SIZE)
	{
		error("cblock.SerializeAsString().size() > MAX_BLOCK_SIZE");
		bRollback = true;
		return false;
	}

	if (CalcBlockHeaderMerkle(cblock) != cblock.merkleroot())
	{
		error("CalcBlockHeaderMerkle(cblock) != cblock.merkleroot()");
		bRollback = true;
		return false;
	}

	
	for (int i = 0; i < cblock.txs_size(); i++)
	{
		CTransaction tx = cblock.txs(i);
		if (!checkTransaction(tx))
		{
			error("checkTransaction(tx)");
			bRollback = true;
			return false;
		}

		bool iscb = CheckTransactionType(tx) == kTransactionType_Fee || CheckTransactionType(tx) == kTransactionType_Award;

		if (iscb && 0 == tx.signprehash_size())
		{
			continue;
		}

		std::string bestChainHash;
		db_status = pRocksDb->GetBestChainHash(txn, bestChainHash);
        if (db_status != 0) {
			error(" pRocksDb->GetBestChainHash");
			bRollback = true;
            return false;
        }
		bool isBestChainHash = bestChainHash.size() != 0;
		if (! isBestChainHash && iscb && 0 == cblock.txs_size())
		{
			continue;
		}

		if (0 == cblock.txs_size() && ! isBestChainHash)
		{
			error("0 == cblock.txs_size() && ! isBestChainHash");
			bRollback = true;
			return false;
		}

		if (isBestChainHash)
		{
			int verifyPreHashCount = 0;
			std::string txBlockHash;

            std::string txHashStr;
            
            for (int i = 0; i < cblock.txs_size(); i++)
            {
                CTransaction transaction = cblock.txs(i);
                if ( CheckTransactionType(transaction) == kTransactionType_Tx)
                {
                    CTransaction copyTx = transaction;
                    for (int i = 0; i != copyTx.vin_size(); ++i)
                    {
                        CTxin * txin = copyTx.mutable_vin(i);
                        txin->clear_scriptsig();
                    }

                    copyTx.clear_signprehash();
                    copyTx.clear_hash();

                    std::string serCopyTx = copyTx.SerializeAsString();

                    size_t encodeLen = serCopyTx.size() * 2 + 1;
                    unsigned char encode[encodeLen] = {0};
                    memset(encode, 0, encodeLen);
                    long codeLen = base64_encode((unsigned char *)serCopyTx.data(), serCopyTx.size(), encode);
                    std::string encodeStr( (char *)encode, codeLen );

                    txHashStr = getsha256hash(encodeStr);
                }
            }

			if (! VerifyTransactionSign(tx, verifyPreHashCount, txBlockHash, txHashStr))
			{
				error("VerifyTransactionSign");
				bRollback = true;
				return false;
			}

			if (verifyPreHashCount < g_MinNeedVerifyPreHashCount)
			{
				error("verifyPreHashCount < g_MinNeedVerifyPreHashCount");
				bRollback = true;
				return false;
			}
		}

		std::string redempUtxoStr;
		for (int i = 0; i < cblock.txs_size(); i++)
		{
			CTransaction transaction = cblock.txs(i);
			if ( CheckTransactionType(transaction) == kTransactionType_Tx)
			{
				CTransaction copyTx = transaction;

				nlohmann::json txExtra = nlohmann::json::parse(copyTx.extra());
				std::string txType = txExtra["TransactionType"].get<std::string>();

				if (txType == TXTYPE_REDEEM)
				{
					

					nlohmann::json txInfo = txExtra["TransactionInfo"].get<nlohmann::json>();
					redempUtxoStr = txInfo["RedeemptionUTXO"].get<std::string>();
				}
			}
		}

		std::vector< std::string > signBase58Addrs;
		for (int i = 0; i < cblock.txs_size(); i++)
		{
			CTransaction transaction = cblock.txs(i);
			if ( CheckTransactionType(transaction) == kTransactionType_Tx)
			{
				
				for (int k = 0; k < transaction.signprehash_size(); k++) 
                {
                    char buf[2048] = {0};
                    size_t buf_len = sizeof(buf);
                    GetBase58Addr(buf, &buf_len, 0x00, transaction.signprehash(k).pub().c_str(), transaction.signprehash(k).pub().size());
					std::string bufStr(buf);
					signBase58Addrs.push_back( bufStr );
                }
			}
		}

		for (int i = 0; i < cblock.txs_size(); i++)
		{
			CTransaction transaction = cblock.txs(i);
			if ( CheckTransactionType(transaction) == kTransactionType_Fee)
			{
				
				if( signBase58Addrs.size() != (size_t)transaction.vout_size() )
				{
					error("signBase58Addrs.size() != (size_t)transaction.vout_size()");
					bRollback = true;
					return false;
				}

				
				for(int l = 0; l < transaction.vout_size(); l++)
				{
					CTxout txout = transaction.vout(l);	
					auto iter = find(signBase58Addrs.begin(), signBase58Addrs.end(), txout.scriptpubkey());
					if( iter == signBase58Addrs.end() )
					{
						error("iter == signBase58Addrs.end()");
						bRollback = true;
						return false;
					}
				}
			}
			else if ( CheckTransactionType(transaction) == kTransactionType_Award )
			{
				uint64_t awardAmountTotal = 0;
				for (auto & txout : transaction.vout())
				{
					awardAmountTotal += txout.value();
				}

				if (awardAmountTotal > g_MaxAwardTotal)
				{
					error("awardAmountTotal error !");
					bRollback = true;
					return false;
				}
			}
		}
	}

	return true;
}


std::string CalcBlockHeaderMerkle(const CBlock & cblock)
{
	std::string merkle;
	if (cblock.txs_size() == 0)
	{
		return merkle;
	}

	std::vector<std::string> vTxHashs;
	for (int i = 0; i != cblock.txs_size(); ++i)
	{
		CTransaction tx = cblock.txs(i);
		vTxHashs.push_back(tx.hash());
	}

	unsigned int j = 0, nSize;
    for (nSize = cblock.txs_size(); nSize > 1; nSize = (nSize + 1) / 2)
	{
        for (unsigned int i = 0; i < nSize; i += 2)
		{
            unsigned int i2 = MIN(i+1, nSize-1);

			std::string data1 = vTxHashs[j + i];
			std::string data2 = vTxHashs[j + i2];
			data1 = getsha256hash(data1);
			data2 = getsha256hash(data2);

			vTxHashs.push_back(getsha256hash(data1 + data2));
        }

        j += nSize;
    }

	merkle = vTxHashs[vTxHashs.size() - 1];

	return merkle;
}

void CalcBlockMerkle(CBlock & cblock)
{
	if (cblock.txs_size() == 0)
	{
		return;
	}

	cblock.set_merkleroot(CalcBlockHeaderMerkle(cblock));
}

CBlock CreateBlock(const CTransaction & tx, const std::shared_ptr<TxMsg>& SendTxMsg)
{
	CBlock cblock;

	uint64_t time = Singleton<TimeUtil>::get_instance()->getTimestamp();
    cblock.set_time(time);
	cblock.set_version(0);

	nlohmann::json txExtra = nlohmann::json::parse(tx.extra());
	int NeedVerifyPreHashCount = txExtra["NeedVerifyPreHashCount"].get<int>();
	std::string txType = txExtra["TransactionType"].get<std::string>();

	
	nlohmann::json blockExtra;
	blockExtra["NeedVerifyPreHashCount"] = NeedVerifyPreHashCount;

	if (txType == TXTYPE_TX)
	{
		blockExtra["TransactionType"] = TXTYPE_TX;
	}
	else if (txType == TXTYPE_PLEDGE)
	{
		nlohmann::json txInfoTmp = txExtra["TransactionInfo"].get<nlohmann::json>();

		nlohmann::json blockTxInfo;
		blockTxInfo["PledgeAmount"] = txInfoTmp["PledgeAmount"].get<int>();

		blockExtra["TransactionType"] = TXTYPE_PLEDGE;
		blockExtra["TransactionInfo"] = blockTxInfo;
	}
	else if (txType == TXTYPE_REDEEM)
	{
		nlohmann::json txInfoTmp = txExtra["TransactionInfo"].get<nlohmann::json>();

		nlohmann::json blockTxInfo;
		blockTxInfo["RedeemptionUTXO"] = txInfoTmp["RedeemptionUTXO"].get<std::string>();

		blockExtra["TransactionType"] = TXTYPE_REDEEM;
		blockExtra["TransactionInfo"] = blockTxInfo;
	}

	cblock.set_extra( blockExtra.dump() );

    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{

	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	std::string bestChainHash;
	db_status = pRocksDb->GetBestChainHash(txn, bestChainHash);
    if (db_status != 0) {
       
    }
	if (bestChainHash.size() == 0)
	{
		cblock.set_prevhash(std::string(64, '0'));
		cblock.set_height(0);
	}
	else
	{
		cblock.set_prevhash(bestChainHash);
		unsigned int preheight = 0;
		db_status = pRocksDb->GetBlockHeightByBlockHash(txn, bestChainHash, preheight);
		if (db_status != 0) {
			error("CreateBlock GetBlockHeightByBlockHash");
		}
		cblock.set_height(preheight + 1);
	}

	CTransaction * tx0 = cblock.add_txs();
	*tx0 = tx;

	if (ENCOURAGE_TX && CheckTransactionType(tx) == kTransactionType_Tx)
	{
		debug("Crreate Encourage TX ... ");

		CTransaction workTx = CreateWorkTx(*tx0);
		CTransaction * tx1 = cblock.add_txs();
		*tx1 = workTx;

        if (get_extra_award_height()) {
            debug("Crreate Encourage TX 2 ... ");
            CTransaction workTx2 = CreateWorkTx(*tx0, true, SendTxMsg);
            CTransaction * txadd2 = cblock.add_txs();
            *txadd2 = workTx2;
        }
	}

	CalcBlockMerkle(cblock);

	std::string serBlockHeader = cblock.SerializeAsString();
	cblock.set_hash(getsha256hash(serBlockHeader));

	return cblock;
}

bool AddBlock(const CBlock & cblock, bool isSync)
{
	unsigned int preheight;
    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		
		return false;
	}

	bool bRollback = true;
	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, bRollback);
	};

	db_status = pRocksDb->GetBlockHeightByBlockHash(txn, cblock.prevhash(), preheight);
    if (db_status != 0) {
		bRollback = true;
        return false;
    }

	CBlockHeader block;
	block.set_hash(cblock.hash());
	block.set_prevhash(cblock.prevhash());
	block.set_time(cblock.time());
	block.set_height(preheight +1);

	unsigned int top = 0;
	if (pRocksDb->GetBlockTop(txn, top) != 0)
	{
		bRollback = true;
		return false;
	}

	
	bool is_mainblock = false;
	if (block.height() > top)  
	{
		is_mainblock = true;
		db_status = pRocksDb->SetBlockTop(txn, block.height());
        if (db_status != 0) {
			bRollback = true;
            return false;
        }
		db_status = pRocksDb->SetBestChainHash(txn, block.hash());
        if (db_status != 0) 
		{
			bRollback = true;
            return false;
        }
	}
	else if (block.height() == top)
	{
		std::string strBestChainHash;
		if (pRocksDb->GetBestChainHash(txn, strBestChainHash) != 0)
		{
			bRollback = true;
			return false;
		}

		std::string strBestChainHeader;
		if (pRocksDb->GetBlockByBlockHash(txn, strBestChainHash, strBestChainHeader) != 0)
		{
			bRollback = true;
			return false;
		}

		CBlock bestChainBlockHeader;
		bestChainBlockHeader.ParseFromString(strBestChainHeader);

		if (cblock.time() < bestChainBlockHeader.time())
		{
			is_mainblock = true;
			db_status = pRocksDb->SetBestChainHash(txn, block.hash());
            if (db_status != 0) 
			{
				bRollback = true;
                return false;
            }
		}
	}
	else if(block.height() < top)
	{
		std::string main_hash;
		pRocksDb->GetBlockHashByBlockHeight(txn, block.height(), main_hash);
		std::string main_block_str;
		if (pRocksDb->GetBlockByBlockHash(txn, main_hash, main_block_str) != 0)
		{
			bRollback = true;
			return false;
		}
		CBlock main_block;
		main_block.ParseFromString(main_block_str);	
		if (cblock.time() < main_block.time())
		{
			is_mainblock = true;
		}
	}

	
	


	db_status = pRocksDb->SetBlockHeightByBlockHash(txn, block.hash(), block.height());
    if (db_status != 0) {
		bRollback = true;
        return false;
    }
	db_status = pRocksDb->SetBlockHashByBlockHeight(txn, block.height(), block.hash(), is_mainblock);
    if (db_status != 0) {
		bRollback = true;
        return false;
    }
	db_status = pRocksDb->SetBlockHeaderByBlockHash(txn, block.hash(), block.SerializeAsString());
    if (db_status != 0) {
		bRollback = true;
        return false;
    }
	db_status = pRocksDb->SetBlockByBlockHash(txn, block.hash(), cblock.SerializeAsString());
    if (db_status != 0) {
		bRollback = true;
        return false;
    }

	
	bool isPledge = false;
	bool isRedeem = false;
	std::string redempUtxoStr;

	nlohmann::json extra = nlohmann::json::parse(cblock.extra());
	std::string txType = extra["TransactionType"].get<std::string>();
	if (txType == TXTYPE_PLEDGE)
	{
		isPledge = true;
	}
	else if (txType == TXTYPE_REDEEM)
	{
		isRedeem = true;
		nlohmann::json txInfo = extra["TransactionInfo"].get<nlohmann::json>();
		redempUtxoStr = txInfo["RedeemptionUTXO"].get<std::string>();
	}
	
	
	uint64_t totalGasFee = 0;
	for (int txCount = 0; txCount < cblock.txs_size(); txCount++)
	{
		CTransaction tx = cblock.txs(txCount);
		if ( CheckTransactionType(tx) == kTransactionType_Fee)
		{
			for (int j = 0; j < tx.vout_size(); j++)
			{
				CTxout txout = tx.vout(j);
				totalGasFee += txout.value();
			}
		}
	}

	if(totalGasFee == 0 && !isRedeem)
	{
		bRollback = true;
		return false;
	}

	for (int i = 0; i < cblock.txs_size(); i++)
	{
		CTransaction tx = cblock.txs(i);
		bool isTx = false;
		if (CheckTransactionType(tx) == kTransactionType_Tx)
		{
			isTx = true;
		}

		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);

			if (isPledge && isTx)
			{
				if ( !txout.scriptpubkey().compare(VIRTUAL_ACCOUNT_PLEDGE) )
				{
					db_status = pRocksDb->SetPledgeAddresses(txn, TxHelper::GetTxOwner(tx)[0]);
					if (db_status != 0 && db_status != pRocksDb->ROCKSDB_IS_EXIST)
					{
						bRollback = true;
						return false;
					}

					db_status = pRocksDb->SetPledgeAddressUtxo(txn, TxHelper::GetTxOwner(tx)[0], tx.hash()); 
					if (db_status != 0)
					{
						bRollback = true;
						return false;
					}
				}
			}

			if ( txout.scriptpubkey().compare(VIRTUAL_ACCOUNT_PLEDGE) )
			{
				db_status = pRocksDb->SetUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash());
				if (db_status != 0 && db_status != pRocksDb->ROCKSDB_IS_EXIST) 
				{
					bRollback = true;
					return false;
				}	
			}
		}

		db_status = pRocksDb->SetTransactionByHash(txn, tx.hash(), tx.SerializeAsString());
        if (db_status != 0) {
			bRollback = true;
            return false;
        }
		db_status = pRocksDb->SetBlockHashByTransactionHash(txn, tx.hash(), cblock.hash());
        if (db_status != 0) {
			bRollback = true;
            return false;
        }

		std::vector<std::string> vPledgeUtxoHashs;
		if (isRedeem && isTx)
		{
			db_status = pRocksDb->GetPledgeAddressUtxo(txn, TxHelper::GetTxOwner(tx)[0], vPledgeUtxoHashs);
			if (db_status != 0) 
			{
				bRollback = true;
				return false;
			}			
		}

		
		if (CheckTransactionType(tx) == kTransactionType_Tx)
		{
			
			std::set<std::pair<std::string, std::string>> utxoAddrSet; 
			for (auto & txin : tx.vin())
			{
				std::string addr = GetBase58Addr(txin.scriptsig().pub());
				std::vector<std::string> utxoHashs;
				db_status = pRocksDb->GetUtxoHashsByAddress(txn, addr, utxoHashs);
				if (db_status != 0) 
				{
					bRollback = true;
					return false;
				}

				
				if (utxoHashs.end() == find(utxoHashs.begin(), utxoHashs.end(), txin.prevout().hash() ) )
				{
					
					if (txin.prevout().hash() != redempUtxoStr)
					{
						bRollback = true;
						return false;
					}
					else
					{
						continue;
					}
				}

				std::pair<std::string, std::string> utxoAddr = make_pair(txin.prevout().hash(), addr);
				utxoAddrSet.insert(utxoAddr);
			}

			for (auto & utxoAddr : utxoAddrSet) 
			{
				std::string utxo = utxoAddr.first;
				std::string addr = utxoAddr.second;


				db_status = pRocksDb->RemoveUtxoHashsByAddress(txn, addr, utxo);
				if (db_status != 0)
				{
					bRollback = true;
					return false;
				}

				
				uint64_t amount = TxHelper::GetUtxoAmount(utxo, addr);
				
				int64_t balance = 0;
				db_status = pRocksDb->GetBalanceByAddress(txn, addr, balance);
				if (db_status != 0) 
				{
					error("AddBlock:GetBalanceByAddress");
					bRollback = true;
					return false;
				}

				balance -= amount;

				if(balance < 0)
				{
					error("balance < 0");
					bRollback = true;
					return false;
				}

				db_status = pRocksDb->SetBalanceByAddress(txn, addr, balance);
				if (db_status != 0) 
				{
					bRollback = true;
					return false;
				}
			}

			if (isRedeem)
			{
				std::string addr = TxHelper::GetTxOwner(tx)[0];
				db_status = pRocksDb->RemovePledgeAddressUtxo(txn, addr, redempUtxoStr);
				if (db_status != 0)
				{
					bRollback = true;
					return false;
				}
				std::vector<string> utxoes;
				db_status = pRocksDb->GetPledgeAddressUtxo(txn, addr, utxoes);
				if (db_status != 0)
				{
					bRollback = true;
					return false;
				}
				if(utxoes.size() == 0)
				{
					db_status = pRocksDb->RemovePledgeAddresses(txn, addr);
					if (db_status != 0)
					{
						bRollback = true;
						return false;
					}
				}
			}
		}

		
		if ( CheckTransactionType(tx) == kTransactionType_Tx)
		{	
			for (int j = 0; j < tx.vout_size(); j++)
			{
				
				CTxout txout = tx.vout(j);
				std::string vout_address = txout.scriptpubkey();
				int64_t balance = 0;
				db_status = pRocksDb->GetBalanceByAddress(txn, vout_address, balance);
				if (db_status != 0) 
				{
					info("AddBlock:GetBalanceByAddress");
				}
				balance += txout.value();
				db_status = pRocksDb->SetBalanceByAddress(txn, vout_address, balance);
				if (db_status != 0) 
				{
					bRollback = true;
					return false;
				}

				if (isRedeem && 
					tx.vout_size() == 2 && 
					tx.vout(0).scriptpubkey() == tx.vout(1).scriptpubkey())
				{
					if (j == 0)
					{
						db_status = pRocksDb->SetAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash());
						if (db_status != 0) {
							bRollback = true;
							return false;
						}
					}
				}
				else
				{
					db_status = pRocksDb->SetAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash());
					if (db_status != 0) {
						bRollback = true;
						return false;
					}
				}
			}
		}
		else
		{
			for (int j = 0; j < tx.vout_size(); j++)
			{
				CTxout txout = tx.vout(j);
				int64_t value = 0;
				db_status = pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value);
				if (db_status != 0) {
					info("AddBlock:GetBalanceByAddress");
				}
				value += txout.value();
				db_status = pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), value);
				if (db_status != 0) {
					bRollback = true;
					return false;
				}
				db_status = pRocksDb->SetAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash());
				if (db_status != 0) {
					bRollback = true;
					return false;
				}
			}
		}

		
		if ( CheckTransactionType(tx) == kTransactionType_Award)
		{
			for (int j = 0; j < tx.vout_size(); j++)
			{
				CTxout txout = tx.vout(j);

				uint64_t awardTotal = 0;
				
				
				
				
				
				pRocksDb->GetAwardTotal(txn, awardTotal);

				awardTotal += txout.value();
				if ( 0 != pRocksDb->SetAwardTotal(txn, awardTotal) )
				{
					bRollback = true;
					return false;
				}
			}
		}
	}

	if( pRocksDb->TransactionCommit(txn) )
	{
		bRollback = true;
		return false;
	}	
	return true;
}

void CalcTransactionHash(CTransaction & tx)
{
	std::string hash = tx.hash();
	if (hash.size() != 0)
	{
		return;
	}

	CTransaction copyTx = tx;

	copyTx.clear_signprehash();

	std::string serTx = copyTx.SerializeAsString();

	hash = getsha256hash(serTx);
	tx.set_hash(hash);
}

bool ContainSelfSign(const CTransaction & tx)
{
	for (int i = 0; i != tx.signprehash_size(); ++i)
	{
		CSignPreHash signPreHash = tx.signprehash(i);

		char pub[2045] = {0};
		size_t pubLen = sizeof(pub);
		GetBase58Addr(pub, &pubLen, 0x00, signPreHash.pub().c_str(), signPreHash.pub().size());

		if (g_AccountInfo.isExist(pub))
		{
			info("Signer [%s] Has Signed !!!", pub);
			return true;
		}
	}
	return false;
}

bool VerifySignPreHash(const CSignPreHash & signPreHash, const std::string & serTx)
{
	int pubLen = signPreHash.pub().size();
	char * rawPub = new char[pubLen * 2 + 2]{0};
	encode_hex(rawPub, signPreHash.pub().c_str(), pubLen);

	ECDSA<ECP, SHA1>::PublicKey publicKey;
	std::string sPubStr;
	sPubStr.append(rawPub, pubLen * 2);
	SetPublicKey(publicKey, sPubStr);

	delete [] rawPub;

	return VerifyMessage(publicKey, serTx, signPreHash.sign());
}

bool VerifyScriptSig(const CScriptSig & scriptSig, const std::string & serTx)
{
	std::string addr = GetBase58Addr(scriptSig.pub());
	

	int pubLen = scriptSig.pub().size();
	char * rawPub = new char[pubLen * 2 + 2]{0};
	encode_hex(rawPub, scriptSig.pub().c_str(), pubLen);

	ECDSA<ECP, SHA1>::PublicKey publicKey;
	std::string sPubStr;
	sPubStr.append(rawPub, pubLen * 2);
	SetPublicKey(publicKey, sPubStr);

	delete [] rawPub;

	return VerifyMessage(publicKey, serTx, scriptSig.sign());

	
	
	
	
	

	
}

bool isRedeemTx(const CTransaction &tx)
{
	std::vector<std::string> txOwners = TxHelper::GetTxOwner(tx);
	for (int i = 0; i < tx.vout_size(); ++i)
	{
		CTxout txout = tx.vout(i);
		if (txOwners.end() == find(txOwners.begin(), txOwners.end(), txout.scriptpubkey()))
		{
			return false;
		}
	}
	return true;
}

bool VerifyTransactionSign(const CTransaction & tx, int & verifyPreHashCount, std::string & txBlockHash, std::string txHash)
{
	
    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		return false;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	if ( CheckTransactionType(tx) == kTransactionType_Tx)
	{
		CTransaction copyTx = tx;
		for (int i = 0; i != copyTx.vin_size(); ++i)
		{
			CTxin * txin = copyTx.mutable_vin(i);
			txin->clear_scriptsig();
		}

		copyTx.clear_signprehash();
		copyTx.clear_hash();

		std::string serCopyTx = copyTx.SerializeAsString();
		size_t encodeLen = serCopyTx.size() * 2 + 1;
		unsigned char encode[encodeLen] = {0};
		memset(encode, 0, encodeLen);
		long codeLen = base64_encode((unsigned char *)serCopyTx.data(), serCopyTx.size(), encode);
		std::string encodeStr( (char *)encode, codeLen );

		std::string txHashStr = getsha256hash(encodeStr);
		txBlockHash = txHashStr;

		if(0 != txHashStr.compare(txHash))
		{
			return false;
		}
		
		for (int i = 0; i != tx.vin_size(); ++i)
		{
			CTxin txin = tx.vin(i);
			if (! VerifyScriptSig(txin.scriptsig(), txHash))
			{
				printf("Verify TX InputSign failed ... \n");
				return false;
			}
		}
		
		std::vector<std::string> owner_pledge_utxo;
		if (isRedeemTx(tx))
		{
			std::vector<std::string> txOwners = TxHelper::GetTxOwner(tx);
			for (auto i : txOwners)
			{
				std::vector<string> utxos;
				if (0 != pRocksDb->GetPledgeAddressUtxo(txn, i, utxos))
				{
					printf("GetPledgeAddressUtxo failed ... \n");
					return false;
				}

				std::for_each(utxos.begin(), utxos.end(),
						[&](std::string &s){ s = s + "_" + i;}
				);

				std::vector<std::string> tmp_owner = owner_pledge_utxo;
				std::sort(utxos.begin(), utxos.end());
				std::set_union(utxos.begin(),utxos.end(),tmp_owner.begin(),tmp_owner.end(),std::back_inserter(owner_pledge_utxo));
				std::sort(owner_pledge_utxo.begin(), owner_pledge_utxo.end());
			}
		}

		std::vector<std::string> owner_utxo_tmp = TxHelper::GetUtxosByAddresses(TxHelper::GetTxOwner(tx));
		std::sort(owner_utxo_tmp.begin(), owner_utxo_tmp.end());

		std::vector<std::string> owner_utxo;
		std::set_union(owner_utxo_tmp.begin(),owner_utxo_tmp.end(),owner_pledge_utxo.begin(),owner_pledge_utxo.end(),std::back_inserter(owner_utxo));
		std::sort(owner_utxo.begin(), owner_utxo.end());

		std::vector<std::string> tx_utxo = TxHelper::GetUtxosByTx(tx);
    	std::sort(tx_utxo.begin(), tx_utxo.end());

		std::vector<std::string> v_union;
		std::set_union(owner_utxo.begin(),owner_utxo.end(),tx_utxo.begin(),tx_utxo.end(),std::back_inserter(v_union));
		std::sort(v_union.begin(), v_union.end());
		

		
		std::set<std::string> tmpSet(v_union.begin(), v_union.end());
		v_union.assign(tmpSet.begin(), tmpSet.end());

		std::vector<std::string> v_diff;
		std::set_difference(v_union.begin(),v_union.end(),owner_utxo.begin(),owner_utxo.end(),std::back_inserter(v_diff));

		if(v_diff.size() > 0)
		{
			printf("VerifyTransactionSign fail. not have enough utxo!!! \n");
			return false;
		}

		// 判断手机或RPC交易时，交易签名者是否是交易发起人
		std::set<std::string> txVinVec;
		for(auto & vin : tx.vin())
		{
			std::string prevUtxo = vin.prevout().hash();
			std::string strTxRaw;
			db_status = pRocksDb->GetTransactionByHash(txn, prevUtxo, strTxRaw);
			if (db_status != 0)
			{
				return false;
			}

			CTransaction prevTx;
			prevTx.ParseFromString(strTxRaw);
			if (prevTx.hash().size() == 0)
			{
				return false;
			}
			
			std::string vinBase58Addr = GetBase58Addr(vin.scriptsig().pub());
			txVinVec.insert(vinBase58Addr);

			std::vector<std::string> txOutVec;
			for (auto & txOut : prevTx.vout())
			{
				txOutVec.push_back(txOut.scriptpubkey());
			}

			if (std::find(txOutVec.begin(), txOutVec.end(), vinBase58Addr) == txOutVec.end())
			{
				return false;
			}
		}

		std::vector<std::string> txOwnerVec;
		SplitString(tx.txowner(), txOwnerVec, "_");

		std::vector<std::string> tmptxVinSet;
		tmptxVinSet.assign(txVinVec.begin(), txVinVec.end());

		std::vector<std::string> ivec(txOwnerVec.size() + tmptxVinSet.size());
		auto iVecIter = set_symmetric_difference(txOwnerVec.begin(), txOwnerVec.end(), tmptxVinSet.begin(), tmptxVinSet.end(), ivec.begin());
		ivec.resize(iVecIter - ivec.begin());

		if (ivec.size()!= 0)
		{
			return false;
		}
	}
	else
	{
		std::string strBestChainHash;
		db_status = pRocksDb->GetBestChainHash(txn, strBestChainHash);
        if (db_status != 0) {
            return false;
        }
		if (strBestChainHash.size() != 0)
		{
			txBlockHash = COIN_BASE_TX_SIGN_HASH;
		}
	}
	
	for (int i = 0; i < tx.signprehash_size(); i++)
	{
		CSignPreHash signPreHash = tx.signprehash(i);
		if (! VerifySignPreHash(signPreHash, txBlockHash))
		{
			printf("VerifyPreHashCount  VerifyMessage failed ... \n");
			return false;
		}

		info("Verify PreBlock HashSign succeed !!! VerifySignedCount[%d] -> %s", verifyPreHashCount + 1, txBlockHash.c_str());
		(verifyPreHashCount)++ ;
	}

	return true;
}

unsigned get_extra_award_height() {
    const unsigned MAX_AWARD = 500000; 
    unsigned award {0};
    unsigned top {0};
    int db_status = 0;
    auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

    db_status = pRocksDb->GetBlockTop(txn, top);
    if (db_status != 0) {
        return 0;
    }
    auto b_height = top;
    if (b_height <= MAX_AWARD) {
        award = 2000;
    }
    return award;
}


bool IsNeedPackage(const CTransaction & tx)
{
	std::vector<std::string> owners = TxHelper::GetTxOwner(tx);
	return IsNeedPackage(owners);
}

bool IsNeedPackage(const std::vector<std::string> & fromAddr)
{
	bool bIsNeedPackage = true;
	for (auto &account : g_AccountInfo.AccountList)
	{
		if (fromAddr.end() != find(fromAddr.begin(), fromAddr.end(), account.first))
		{
			bIsNeedPackage = false;
		}
	}
	return bIsNeedPackage;
}


void new_add_ouput_by_signer(CTransaction &tx, bool bIsAward, const std::shared_ptr<TxMsg>& msg) 
{
    
	nlohmann::json extra = nlohmann::json::parse(tx.extra());
	int needVerifyPreHashCount = extra["NeedVerifyPreHashCount"].get<int>();
	int gasFee = extra["SignFee"].get<int>();
	int packageFee = extra["PackageFee"].get<int>();

    
    std::vector<int> award_list;
    int award = 2000000;
    getNodeAwardList(needVerifyPreHashCount, award_list, award);
    auto award_begin = award_list.begin();
    auto award_end = award_list.end();

	std::vector<std::string> signers;
    std::vector<double> online_time;
	std::vector<uint64_t> num_arr;

	for (int i = 0; i < tx.signprehash_size(); i++)
	{
        if (bIsAward) 
		{
            CSignPreHash txpre = tx.signprehash(i);
            int pubLen = txpre.pub().size();
            char *rawPub = new char[pubLen * 2 + 2]{0};
            encode_hex(rawPub, txpre.pub().c_str(), pubLen);
            ECDSA<ECP, SHA1>::PublicKey publicKey;
            std::string sPubStr;
            sPubStr.append(rawPub, pubLen * 2);
            SetPublicKey(publicKey, sPubStr);
            delete [] rawPub;

            for (int j = 0; j < msg->signnodemsg_size(); j++) 
			{
                std::string ownPubKey;
                GetPublicKey(publicKey, ownPubKey);
                char SignerCstr[2048] = {0};
                size_t SignerCstrLen = sizeof(SignerCstr);
                GetBase58Addr(SignerCstr, &SignerCstrLen, 0x00, ownPubKey.c_str(), ownPubKey.size());
                auto psignNodeMsg = msg->signnodemsg(j);
                std::string signpublickey = psignNodeMsg.signpubkey();
                const char * signpublickeystr = signpublickey.c_str();

                if (!strcmp(SignerCstr, signpublickeystr)) 
				{
                    std::string temp_signature = psignNodeMsg.signmsg();
                    psignNodeMsg.clear_signmsg();
                    std::string message = psignNodeMsg.SerializeAsString();

                    auto re = VerifyMessage(publicKey, message, temp_signature);
                    if (!re) 
					{
                        
                    } 
					else 
					{
                        signers.push_back(signpublickeystr);
                        online_time.push_back(psignNodeMsg.onlinetime());
                    }
                }
            }

        } 
		else 
		{
			bool bIsLocal = false;    
			std::vector<std::string> txOwners = TxHelper::GetTxOwner(tx);
			if (txOwners.size() == 0) 
			{
				continue;
			}

			char buf[2048] = {0};
            size_t buf_len = sizeof(buf);

			CSignPreHash signPreHash = tx.signprehash(i);
			GetBase58Addr(buf, &buf_len, 0x00, signPreHash.pub().c_str(), signPreHash.pub().size());
			signers.push_back(buf);

			if (txOwners[0] == buf)
			{
				bIsLocal = true;
			}

			uint64_t num = 0;
			
            if (i == 0)
            {
				if (bIsLocal)
				{
					num = 0;
				}
				else
				{
					num = packageFee;
				}
            }
            else
            {
                if (!bIsAward) 
				{
					num = gasFee;
                } 
				else 
				{
                    num = (*award_begin);
                    ++award_begin;
                    if (award_begin == award_end) break;
                }
            }

            num_arr.push_back(num);
        }
	}

    if (bIsAward) 
	{
        num_arr.push_back(0); 
        a_award::AwardAlgorithm ex_award;
        ex_award.Build(needVerifyPreHashCount, signers, online_time);
        auto dis_award = ex_award.GetDisAward();
        for (auto v : dis_award) 
		{
            CTxout * txout = tx.add_vout();
            txout->set_value(v.first);
            txout->set_scriptpubkey(v.second);
        }
        
        
    } 
	else 
	{
        for (int i = 0; i < needVerifyPreHashCount; ++i)
        {
            CTxout * txout = tx.add_vout();
            txout->set_value(num_arr[i]);
            txout->set_scriptpubkey(signers[i]);
            info("Transaction signer [%s]", signers[i].c_str());
        }
    }
}

CTransaction CreateWorkTx(const CTransaction & tx, bool bIsAward, const std::shared_ptr<TxMsg>& psignNodeMsg ) 
{
    CTransaction retTx;
    if (tx.vin_size() == 0) {
        return retTx;
    }
	std::string owner = TxHelper::GetTxOwner(tx)[0];
    g_AccountInfo.SetKeyByBs58Addr(g_privateKey, g_publicKey, owner.c_str());
    retTx = tx;

    retTx.clear_vin();
    retTx.clear_vout();
    retTx.clear_hash();


    int db_status = 0;
	(void)db_status;
    auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

    unsigned int txIndex = 0;
    db_status = pRocksDb->GetTransactionTopByAddress(txn, retTx.txowner(), txIndex);
    if (db_status != 0) {
        
    }

    txIndex++;
    retTx.set_n(txIndex);

	uint64_t time = Singleton<TimeUtil>::get_instance()->getTimestamp();
    retTx.set_time(time);
    retTx.set_ip(net_get_self_node_id());

	CTxin ownerTxin = tx.vin(0);

    CTxin * txin = retTx.add_vin();
    txin->set_nsequence(0xffffffff);
    CTxprevout * prevout = txin->mutable_prevout();
    prevout->set_n(0xffffffff);
    CScriptSig * scriptSig = txin->mutable_scriptsig();
    
    
	*scriptSig = ownerTxin.scriptsig();

    if (!bIsAward) 
	{
        prevout->set_hash(tx.hash());
    }

    new_add_ouput_by_signer(retTx, bIsAward, psignNodeMsg);

    retTx.clear_signprehash();

    std::string serRetTx = retTx.SerializeAsString();
    std::string signature;
    std::string strPub;
    GetSignString(serRetTx, signature, strPub);

    for (int i = 0; i < retTx.vin_size(); i++)
    {
        CTxin * txin = retTx.mutable_vin(i);
        CScriptSig * scriptSig = txin->mutable_scriptsig();

        if (!bIsAward) 
		{
            scriptSig->set_sign(FEE_SIGN_STR);
        } 
		else 
		{
            scriptSig->set_sign(EXTRA_AWARD_SIGN_STR);
        }
        scriptSig->set_pub("");
    }

    CalcTransactionHash(retTx);

    return retTx;
}

void InitAccount(accountinfo *acc, const char *path)
{
	if(NULL == path)
		g_AccountInfo.path =  OWNER_CERT_PATH;
	else
		g_AccountInfo.path =  path;

	if('/' != g_AccountInfo.path[g_AccountInfo.path.size()-1])
		g_AccountInfo.path += "/";

	if(access(g_AccountInfo.path.c_str(), F_OK))
    {
        if(mkdir(g_AccountInfo.path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH))
        {
            assert(false);
            return;
        }
    }

    if(!acc)
	{
		std::cout<<"InitAccount Failed ..."<<std::endl;
        return;
	}
    DIR *dir;
    struct dirent *ptr;

    if ((dir=opendir(g_AccountInfo.path.c_str())) == NULL)
    {
		error("OPEN DIR%s ERROR ..." , g_AccountInfo.path.c_str());
		return;
    }

    while ((ptr=readdir(dir)) != NULL)
    {
        if(strcmp(ptr->d_name,".")==0 || strcmp(ptr->d_name,"..")==0)
		{
            continue;
		}
        else
        {
			debug("type[%d] filename[%s]\n", ptr->d_type, ptr->d_name);
            std::string bs58addr;
            if(0 == memcmp(ptr->d_name, OWNER_CERT_DEFAULT, strlen(OWNER_CERT_DEFAULT)))
            {
				std::string name(ptr->d_name);
				char *p = ptr->d_name + name.find('.') + 1;
				std::string ps(p);
                bs58addr.append(ptr->d_name, p + ps.find('.') - ptr->d_name );
                acc->GenerateKey(bs58addr.c_str(), true);
            }
            else
            {
				std::string name(ptr->d_name);
                bs58addr.append(ptr->d_name, ptr->d_name + name.find('.') - ptr->d_name);
                acc->GenerateKey(bs58addr.c_str(), false);
            }
        }
    }
	closedir(dir);
	return;
}

void GetDefault58Addr(char *buf, size_t len)
{
	string sPubStr;
	GetPublicKey(g_publicKey, sPubStr);
	cstring * pPubKey = cstr_new_buf(sPubStr.c_str(), sPubStr.size());
	size_t buf_len = len;
	GetBase58Addr(buf, &buf_len, 0x00, pPubKey->str, pPubKey->len);
	cstr_free(pPubKey, true);
}



int getNodeAwardList(int consensus, std::vector<int> &award_list, int amount, float coe) {
    using namespace std;

    
    amount = amount*coe; 
    consensus -= 1;
    consensus = consensus == 0 ? 1 : consensus;
    
    int base {amount/consensus}; 
    int surplus = amount - base*consensus; 
    award_list.push_back(amount);
    for (auto i = 1; i <= consensus; ++i) { 
        award_list.push_back(base);
    }
    award_list[consensus] += surplus;

    
    auto list_end_award {0};
    for (auto i = 1; i < consensus; ++i) {
        award_list[i] -= i;
        list_end_award += i;
    }

    auto temp_consensus = consensus;
    auto diff_value = 10; 
    while (list_end_award > diff_value) {
        if (list_end_award > diff_value && list_end_award < consensus) {
            consensus = diff_value;
        }
        for (auto i = 1; i < consensus; ++i) { 
            award_list[i] += 1; 
        }
        if (list_end_award < consensus) {
            list_end_award -= diff_value;
        } else {
            list_end_award -= consensus-1;
        }

    }

    award_list[temp_consensus] += list_end_award;
    sort(award_list.begin(), award_list.end());

    
    while (award_list[0] <= 0) { 
        for (auto i = 0; i < temp_consensus - 1; ++i) {
            if (award_list[i] <= 0) {
                if (award_list[i] == 0) {
                    award_list[i] = 1;
                    award_list[temp_consensus-1-i] -= 1;
                } else {
                    award_list[i] = abs(award_list[i]) + 1;
                    award_list[temp_consensus-1-i] -= award_list[i] + (award_list[i] - 1);
                }
            } else {
                break;
            }
        }

        sort(award_list.begin(), award_list.end());
    }

    
    while (award_list[temp_consensus-1] == award_list[temp_consensus-2]) {
        award_list[temp_consensus-1] += 1;
        award_list[temp_consensus-2] -= 1;
        sort(award_list.begin(), award_list.end());
    }

    if (amount == 0) {
        for (auto &v : award_list) {
            v = 0;
        }
    }

    return 1;
}

int RollbackRedeemTx(std::shared_ptr<Rocksdb> pRocksDb, Transaction* txn, CTransaction &tx)
{
	ca_console ResBlockColor(kConsoleColor_Green, kConsoleColor_Black, true);

	nlohmann::json txExtra = nlohmann::json::parse(tx.extra());
	nlohmann::json txInfo = txExtra["TransactionInfo"].get<nlohmann::json>();
	std::string redempUtxoStr = txInfo["RedeemptionUTXO"].get<std::string>();

	
	std::vector<std::string> owner_addrs = TxHelper::GetTxOwner(tx);
	std::string txOwner = owner_addrs[0];

	if(CheckTransactionType(tx) == kTransactionType_Tx)
	{
		uint64_t pledgeValue = 0;

		std::string txRaw;
		if (0 != pRocksDb->GetTransactionByHash(txn, redempUtxoStr, txRaw) )
		{
			printf("%sGetTransactionByHash failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -1;
		}

		CTransaction utxoTx;
		utxoTx.ParseFromString(txRaw);

		for (int j = 0; j < utxoTx.vout_size(); j++)
		{
			CTxout txOut = utxoTx.vout(j);
			if (txOut.scriptpubkey() == VIRTUAL_ACCOUNT_PLEDGE)
			{
				pledgeValue += txOut.value();
			}
		}

		
		std::vector<std::string> vinUtxos;
		uint64_t vinAmountTotal = 0;
		uint64_t voutAmountTotal = 0;
		for (auto & txin : tx.vin())
		{
			vinUtxos.push_back(txin.prevout().hash());
		}

		auto iter = find(vinUtxos.begin(), vinUtxos.end(), redempUtxoStr);
		if (iter != vinUtxos.end())
		{
			vinUtxos.erase(iter);
		}
		else
		{
			printf("%s Find redempUtxoStr in vinUtxos failed ! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -2;
		}
		
		for (auto & vinUtxo : vinUtxos)
		{
			vinAmountTotal += TxHelper::GetUtxoAmount(vinUtxo, txOwner);
		}
		
		for (auto & txout : tx.vout())
		{
			voutAmountTotal += txout.value();
		}

		
		nlohmann::json extra = nlohmann::json::parse(tx.extra());
		uint64_t signFee = extra["SignFee"].get<int>();
		uint64_t NeedVerifyPreHashCount = extra["NeedVerifyPreHashCount"].get<int>();
		uint64_t packageFee = extra["PackageFee"].get<int>();

		voutAmountTotal += signFee * NeedVerifyPreHashCount;
		voutAmountTotal += packageFee;

		bool bIsUnused = true;
		if (voutAmountTotal != vinAmountTotal)
		{
			uint64_t usable = TxHelper::GetUtxoAmount(redempUtxoStr, txOwner);
			if (voutAmountTotal == vinAmountTotal - usable)
			{
				
				bIsUnused = false;
			}
		}

		for (auto & txin : tx.vin())
		{
			if (txin.prevout().hash() == redempUtxoStr && bIsUnused)
			{
				continue;
			}
			
			if ( 0 != pRocksDb->SetUtxoHashsByAddress(txn, txOwner, txin.prevout().hash()) )
			{
				std::string txRaw;
				if ( 0 != pRocksDb->GetTransactionByHash(txn, txin.prevout().hash(), txRaw) )
				{
					printf("%sGetTransactionByHash failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -2;
				}

				CTransaction vinUtxoTx;
				vinUtxoTx.ParseFromString(txRaw);

				nlohmann::json extra = nlohmann::json::parse(vinUtxoTx.extra());
				std::string txType = extra["TransactionType"].get<std::string>();
				if (txType != TXTYPE_REDEEM)
				{
					printf("%sSetUtxoHashsByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -3;
				}
			}
		}

		int64_t value = 0;
		if ( 0 != pRocksDb->GetBalanceByAddress(txn, txOwner, value) )
		{
			printf("%sGetBalanceByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -2;
		}

		int64_t amount = 0;
		amount = value - pledgeValue;

		
		if ( 0 != pRocksDb->SetBalanceByAddress(txn, txOwner, amount) )
		{
			printf("%sSetBalanceByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -3;
		}

		
		if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txOwner, tx.hash()) )
		{
			printf("%sRemoveAllTransactionByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -4;
		}

		
		if (0 != pRocksDb->SetPledgeAddressUtxo(txn, txOwner, redempUtxoStr) )
		{
			printf("%sSetPledgeAddressUtxo failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -5;
		}

		
		std::vector<std::string> utxoes;
		if (0 != pRocksDb->GetPledgeAddressUtxo(txn, txOwner, utxoes))
		{
			printf("%sGetPledgeAddressUtxo failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -6;
		}

		
		if (utxoes.size() == 1)
		{
			if (0 != pRocksDb->SetPledgeAddresses(txn, txOwner))
			{
				printf("%sSetPledgeAddresses failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -7;
			}
		}

		if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txOwner, tx.hash()))
		{
			printf("%sRemoveUtxoHashsByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -8;
		}
	}
	else if (CheckTransactionType(tx) == kTransactionType_Fee || CheckTransactionType(tx) == kTransactionType_Award)
	{
		uint64_t signFee = 0;
		std::string txOwnerAddr;
		std::string txRaw;
		if (0 != pRocksDb->GetTransactionByHash(txn, redempUtxoStr, txRaw) )
		{
			printf("%sGetTransactionByHash failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
			return -9;
		}

		CTransaction utxoTx;
		utxoTx.ParseFromString(txRaw);

		for (int j = 0; j < utxoTx.vout_size(); j++)
		{
			CTxout txOut = utxoTx.vout(j);
			if (txOut.scriptpubkey() != VIRTUAL_ACCOUNT_PLEDGE)
			{
				txOwnerAddr += txOut.scriptpubkey();
			}
		}

		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);

			if (txout.scriptpubkey() != txOwnerAddr)
			{
				signFee += txout.value();
			}

			int64_t value = 0;
			if ( 0 != pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value) )
			{
				printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -10;
			}
			int64_t amount = value - txout.value();
			if ( 0 != pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), amount) )
			{
				printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -11;
			}
			if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveAllTransactionByAddress  5  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -12;
			}
			if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveUtxoHashsByAddress  2  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -13;
			}
		}

		if (CheckTransactionType(tx) == kTransactionType_Fee)
		{
			int64_t value = 0;
			if ( 0 != pRocksDb->GetBalanceByAddress(txn, txOwnerAddr, value) )
			{
				printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -14;
			}

			signFee += value;

			if ( 0 != pRocksDb->SetBalanceByAddress(txn, txOwnerAddr, signFee) )
			{
				printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -15;
			}
		}
	}

	return 0;
}

int RollbackPledgeTx(std::shared_ptr<Rocksdb> pRocksDb, Transaction* txn, CTransaction &tx)
{
	int db_status = 0;
	std::vector<std::string> owner_addrs = TxHelper::GetTxOwner(tx);
	ca_console ResBlockColor(kConsoleColor_Green, kConsoleColor_Black, true);

	
	std::string addr;
	if (owner_addrs.size() != 0)
	{
		addr = owner_addrs[0]; 
	}
	
	if (CheckTransactionType(tx) == kTransactionType_Tx) 
	{
		if (0 !=  pRocksDb->RemovePledgeAddressUtxo(txn, addr, tx.hash()))
		{
			return -33;
		}

		std::vector<std::string> utxoes;
		pRocksDb->GetPledgeAddressUtxo(txn, addr, utxoes); 
		
		if (utxoes.size() == 0)
		{
			if (0 != pRocksDb->RemovePledgeAddresses(txn, addr))
			{
				return -34;
			}
		}

		
		for (int j = 0; j < tx.vin_size(); j++)
		{
			CTxin txin = tx.vin(j);
			std::string vin_hash = txin.prevout().hash();  
			std::string vin_owner = GetBase58Addr(txin.scriptsig().pub());

			if ( 0 != pRocksDb->SetUtxoHashsByAddress(txn, vin_owner, vin_hash ))
			{
				
				std::string txRaw;
				if ( 0 != pRocksDb->GetTransactionByHash(txn, vin_hash, txRaw) )
				{
					printf("%sGetTransactionByHash  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -17;
				}

				CTransaction vinHashTx;
				vinHashTx.ParseFromString(txRaw);

				nlohmann::json extra = nlohmann::json::parse(vinHashTx.extra());
				std::string txType = extra["TransactionType"];

				if (txType != TXTYPE_REDEEM)
				{
					printf("%sSetUtxoHashsByAddress  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -17;
				}
				else
				{
					continue;
				}	
			}

			
			uint64_t amount = TxHelper::GetUtxoAmount(vin_hash, vin_owner);
			int64_t balance = 0;

			db_status = pRocksDb->GetBalanceByAddress(txn, vin_owner, balance);
			if (db_status != 0) 
			{
				error("AddBlock:GetBalanceByAddress");
			}

			balance += amount;
			db_status = pRocksDb->SetBalanceByAddress(txn, vin_owner, balance);
			if (db_status != 0) 
			{
				return -18;
			}
		}

		
		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);
			
			
			int64_t value = 0;
			if (txout.scriptpubkey() != VIRTUAL_ACCOUNT_PLEDGE)
			{
				if ( 0 != pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value) )
				{
					printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -30;
				}
				int64_t amount = value - txout.value();
				if ( 0 != pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), amount) )
				{
					printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -31;
				}

				if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash()))
				{
					printf("%sRemoveUtxoHashsByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -15;
				}
				if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash()) )
				{
					printf("%sRemoveAllTransactionByAddress  5  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -32;
				}
			}
		}
	}
	else if (CheckTransactionType(tx) == kTransactionType_Fee || CheckTransactionType(tx) == kTransactionType_Award)
	{
		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);
			int64_t value = 0;
			if ( 0 != pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value) )
			{
				printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -30;
			}
			int64_t amount = value - txout.value();
			if ( 0 != pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), amount) )
			{
				printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -31;
			}
			if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveAllTransactionByAddress  5  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -32;
			}
			if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveUtxoHashsByAddress  2  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -18;
			}
		}
	}

	return 0;
}

int RollbackTx(std::shared_ptr<Rocksdb> pRocksDb, Transaction* txn, CTransaction tx)
{
	ca_console ResBlockColor(kConsoleColor_Green, kConsoleColor_Black, true);
	int db_status = 0;
	std::vector<std::string> owner_addrs = TxHelper::GetTxOwner(tx);
	if(CheckTransactionType(tx) == kTransactionType_Tx) 
	{
		
		for (int j = 0; j < tx.vin_size(); j++)
		{
			CTxin txin = tx.vin(j);
			std::string vin_hash = txin.prevout().hash();  
			std::string vin_owner = GetBase58Addr(txin.scriptsig().pub());

			if ( 0 != pRocksDb->SetUtxoHashsByAddress(txn, vin_owner, vin_hash ))
			{
				
				std::string txRaw;
				if ( 0 != pRocksDb->GetTransactionByHash(txn, vin_hash, txRaw) )
				{
					printf("%sGetTransactionByHash  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -17;
				}

				CTransaction vinHashTx;
				vinHashTx.ParseFromString(txRaw);

				nlohmann::json extra = nlohmann::json::parse(vinHashTx.extra());
				std::string txType = extra["TransactionType"];

				if (txType != TXTYPE_REDEEM)
				{
					printf("%sSetUtxoHashsByAddress  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					return -17;
				}
				else
				{
					continue;
				}	
			}

			
			uint64_t amount = TxHelper::GetUtxoAmount(vin_hash, vin_owner);
			int64_t balance = 0;
			db_status = pRocksDb->GetBalanceByAddress(txn, vin_owner, balance);
			if (db_status != 0) {
				error("AddBlock:GetBalanceByAddress");
			}
			balance += amount;
			db_status = pRocksDb->SetBalanceByAddress(txn, vin_owner, balance);
			if (db_status != 0) {
				return -18;
			}
		}
		
		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);
			
			
			int64_t value = 0;
			if ( 0 != pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value) )
			{
				printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -30;
			}
			int64_t amount = value - txout.value();
			if ( 0 != pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), amount) )
			{
				printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -31;
			}

			if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash()))
			{
				printf("%sRemoveUtxoHashsByAddress failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -15;
			}
			if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveAllTransactionByAddress  5  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -32;
			}
		}					
	}	
	else if (CheckTransactionType(tx) == kTransactionType_Fee || CheckTransactionType(tx) == kTransactionType_Award)
	{
		for (int j = 0; j < tx.vout_size(); j++)
		{
			CTxout txout = tx.vout(j);
			int64_t value = 0;
			if ( 0 != pRocksDb->GetBalanceByAddress(txn, txout.scriptpubkey(), value) )
			{
				printf("%sGetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -30;
			}
			int64_t amount = value - txout.value();
			if ( 0 != pRocksDb->SetBalanceByAddress(txn, txout.scriptpubkey(), amount) )
			{
				printf("%sSetBalanceByAddress  3  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -31;
			}
			if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveAllTransactionByAddress  5  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -32;
			}
			if ( 0 != pRocksDb->RemoveUtxoHashsByAddress(txn, txout.scriptpubkey(), tx.hash()) )
			{
				printf("%sRemoveUtxoHashsByAddress  2  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				return -18;
			}
		}
	}
	return 0;
}


int RollbackToHeight(unsigned int height)
{
	ca_console ResBlockColor(kConsoleColor_Green, kConsoleColor_Black, true);
    int db_status = 0;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		std::cout << "(RollbackToHeight) TransactionInit failed !" << std::endl;
		return -1;
	}
	bool bRollback = true;
	unsigned int top = 0;
	db_status = pRocksDb->GetBlockTop(txn, top);
    if (db_status != 0) {
		bRollback = true;
        return -2;
    }
	pRocksDb->TransactionDelete(txn, false);
	if (height >= top)
	{
		bRollback = true;
		return -2;
	}

	while (top > height)
	{
		txn = pRocksDb->TransactionInit();
		if( txn == NULL )
		{
			std::cout << "(RollbackToHeight) TransactionInit failed !" << std::endl;
			return -1;
		}
		bRollback = true;
		ON_SCOPE_EXIT{
			pRocksDb->TransactionDelete(txn, bRollback);
		};
		
		std::vector<std::string> vBlockHashs;
		if ( 0 != pRocksDb->GetBlockHashsByBlockHeight(txn, top, vBlockHashs) )
		{
			printf("%sGetBlockHashsByBlockHeight failed !!! %s\n", ResBlockColor.color().c_str(), ResBlockColor.reset().c_str());
			bRollback = true;
			return -3;
		}

		for (auto blockHash : vBlockHashs)
		{
			CBlock cblock;
			std::string serBlockHeader;

            
            
            uint64_t counts{0};
            pRocksDb->GetTxCount(txn, counts);
            counts--;
            pRocksDb->SetTxCount(txn, counts);
            
            counts = 0;
            pRocksDb->GetGasCount(txn, counts);
            counts--;
            pRocksDb->SetGasCount(txn, counts);
            
            counts = 0;
            pRocksDb->GetAwardCount(txn, counts);
            counts--;
            pRocksDb->SetAwardCount(txn, counts);

			if ( 0 != pRocksDb->GetBlockByBlockHash(txn, blockHash, serBlockHeader) )
			{
				printf("%sGetBlockHeaderByBlockHash failed !!! %s\n", ResBlockColor.color().c_str(), ResBlockColor.reset().c_str());
				bRollback = true;
				return -4;
			}

			cblock.ParseFromString(serBlockHeader);

			bool isRedeem = false;
			bool isPledge = false;
			std::string redempUtxoStr;

			nlohmann::json blockExtra = nlohmann::json::parse(cblock.extra());
			std::string txType = blockExtra["TransactionType"].get<std::string>();
			if (txType == TXTYPE_PLEDGE)
			{
				isPledge = true;
			}
			else if (txType == TXTYPE_REDEEM)
			{
				isRedeem = true;
				nlohmann::json txInfo = blockExtra["TransactionInfo"].get<nlohmann::json>();
				redempUtxoStr = txInfo["RedeemptionUTXO"].get<std::string>();
			}

			for (int i = 0; i < cblock.txs_size(); i++)
			{
				CTransaction tx = cblock.txs(i);
				std::vector<std::string> owner_addrs = TxHelper::GetTxOwner(tx);
				if ( 0 != pRocksDb->DeleteTransactionByHash(txn, tx.hash()) )
				{
					printf("%sDeleteTransactionByHash failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					bRollback = true;
					return -8;
				}

				if ( 0 != pRocksDb->DeleteBlockHashByTransactionHash(txn, tx.hash()) )
				{
					printf("%sDeleteBlockHashByTransactionHash failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
					bRollback = true;
					return -9;
				}
				if(CheckTransactionType(tx) == kTransactionType_Tx)
				{
					for(const auto& addr:owner_addrs)
					{
						if ( 0 != pRocksDb->RemoveAllTransactionByAddress(txn, addr, tx.hash()) )
						{
							std::cout << "addr:" << addr << " hash:" << tx.hash() << std::endl;
							printf("%sRemoveAllTransactionByAddress  1   failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
							bRollback = true;
							return -14;
						}
					}
				}
			}

			CBlockHeader block;
			std::string serBlock;
			if ( 0 != pRocksDb->GetBlockHeaderByBlockHash(txn, blockHash, serBlock) )
			{
				printf("%sGetBlockByBlockHash  2  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				bRollback = true;
				return -20;
			}

			block.ParseFromString(serBlock);

			if ( 0 != pRocksDb->DeleteBlockHeightByBlockHash(txn, blockHash) )
			{
				printf("%sDeleteBlockHeightByBlockHash  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				bRollback = true;
				return -21;
			}

			if ( 0 != pRocksDb->RemoveBlockHashByBlockHeight(txn, top, blockHash) )
			{
				printf("%sRemoveBlockHashByBlockHeight  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				bRollback = true;
				return -22;
			}

			if ( 0 != pRocksDb->DeleteBlockByBlockHash(txn, blockHash) )
			{
				printf("%sDeleteBlockHeaderByBlockHash  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				bRollback = true;
				return -23;
			}

			if ( 0 != pRocksDb->DeleteBlockHeaderByBlockHash(txn, blockHash) )
			{
				printf("%sDeleteBlockByBlockHash  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
				bRollback = true;
				return -24;
			}

			for (int i = 0; i < cblock.txs_size(); i++)
			{
				CTransaction tx = cblock.txs(i);
				if (isPledge)
				{
					if (0 != RollbackPledgeTx(pRocksDb, txn, tx) )
					{
						bRollback = true;
						return -25;
					}
				}
				else if (isRedeem)
				{
					if (0 != RollbackRedeemTx(pRocksDb, txn, tx))
					{
						bRollback = true;
						return -26;
					}
				}
				else
				{
					int ret = RollbackTx(pRocksDb, txn, tx);
					if( ret != 0)
					{
						bRollback = true;
						return ret;
					}
				}
			}
		}
		
		
		if ( 0 != pRocksDb->SetBlockTop(txn, --top) )
			printf("%sSetBlockTop  failed !!! %s\n", ResBlockColor.color().c_str(),  ResBlockColor.reset().c_str());
		
		std::string besthash;
		pRocksDb->GetBlockHashByBlockHeight(txn, top, besthash);		
		db_status = pRocksDb->SetBestChainHash(txn, besthash);
        if (db_status != 0) 
		{
			std::cout << "rollback SetBestChainHash error" << std::endl;
        }	

		if( pRocksDb->TransactionCommit(txn) )
		{
			bRollback = true;
			std::cout << "(RollbackToHeight) TransactionCommit failed !" << std::endl;
			return -33;
		}
	}
	

	ca_console redColor(kConsoleColor_Red, kConsoleColor_Black, true);
	std::cout << redColor.color() << "Rollback to block " << height << redColor.reset() << std::endl;

	return 0;
}




bool ExitGuardian()
{
	std::string name = "ebpc_daemon";
	char cmd[128];
	memset(cmd, 0, sizeof(cmd));

	sprintf(cmd, "ps -ef | grep %s | grep -v grep | awk '{print $2}' | xargs kill -9 ",name.c_str());
	system(cmd);
	return true;
}


void HandleBuileBlockBroadcastMsg( const std::shared_ptr<BuileBlockBroadcastMsg>& msg, const MsgData& msgdata )
{
	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		error("HandleBuileBlockBroadcastMsg IsVersionCompatible");
		return ;
	}

	std::string serBlock = msg->blockraw();
	CBlock cblock;
	cblock.ParseFromString(serBlock);

	MagicSingleton<BlockPoll>::GetInstance()->Add(Block(cblock));
}


int BuildBlock(std::string &recvTxString, const std::shared_ptr<TxMsg>& SendTxMsg)
{
	CTransaction tx;
	tx.ParseFromString(recvTxString);

	if (! checkTransaction(tx))
	{
		error("BuildBlock checkTransaction");
		return -1;
	}
	CBlock cblock = CreateBlock(tx, SendTxMsg);
	std::string serBlock = cblock.SerializeAsString();
	
	bool ret = VerifyBlockHeader(cblock);

	if(!ret)
	{
		error("BuildBlock VerifyBlockHeader fail!!!");
		return -1;
	}
	if(MagicSingleton<BlockPoll>::GetInstance()->CheckConflict(cblock))
	{
		error("BuildBlock BlockPoll have CheckConflict!!!");
		return -1;
	}

	bool succ = MagicSingleton<BlockPoll>::GetInstance()->Add(Block(cblock));
	if(!succ)
	{
		return -1;
	}
	BuileBlockBroadcastMsg buileBlockBroadcastMsg;
	buileBlockBroadcastMsg.set_version(getVersion());
	buileBlockBroadcastMsg.set_blockraw(serBlock);

	net_broadcast_message<BuileBlockBroadcastMsg>(buileBlockBroadcastMsg);
	info("BuildBlock BuileBlockBroadcastMsg");
	return 0;
}

int IsLinuxVersionCompatible(const std::vector<std::string> & vRecvVersion)
{
	if (vRecvVersion.size() == 0)
	{
		std::cout << "(linux)版本错误：-1" << std::endl;
		return -1;
	}
	std::string ownerVersion = getVersion();
	std::vector<std::string> vOwnerVersion;
	SplitString(ownerVersion, vOwnerVersion, "_");

	if (vOwnerVersion.size() != 3)
	{
		std::cout << "(linux)版本错误：-2" << std::endl;
		return -2;
	}

	
	std::vector<std::string> vOwnerVersionNum;
	SplitString(vOwnerVersion[1], vOwnerVersionNum, ".");
	if (vOwnerVersionNum.size() == 0)
	{
		std::cout << "(linux)版本错误：-3" << std::endl;
		return -3;
	}

	std::vector<std::string> vRecvVersionNum;
	SplitString(vRecvVersion[1], vRecvVersionNum, ".");
	if (vRecvVersionNum.size() != vOwnerVersionNum.size())
	{
		std::cout << "(linux)版本错误：-4" << std::endl;
		return -4;
	}

	for (size_t i = 0; i < vRecvVersionNum.size(); i++)
	{
		if (vRecvVersionNum[i] < vOwnerVersionNum[i])
		{
			std::cout << "(linux)版本错误：-5" << std::endl;
			return -5;
		}
	}

	if (g_testflag == 1)
	{
		if (vRecvVersion[2] != "t")
		{
			std::cout << "(linux)版本错误：-6" << std::endl;
			return -6;
		}
	}
	else
	{
		if (vRecvVersion[2] != "p")
		{
			std::cout << "(linux)版本错误：-7" << std::endl;
			return -7;
		}
	}

	return 0;
}

int IsOtherVersionCompatible(const std::string & vRecvVersion, bool bIsAndroid)
{
	if (vRecvVersion.size() == 0)
	{
		std::cout << "(other) 版本错误: -1" << std::endl;
		return -1;
	}

	std::vector<std::string> vRecvVersionNum;
	SplitString(vRecvVersion, vRecvVersionNum, ".");
	if (vRecvVersionNum.size() != 3)
	{
		std::cout << "(other) 版本错误: -2" << std::endl;
		return -2;
	}

	std::string ownerVersion;
	if (bIsAndroid)
	{
		ownerVersion = g_AndroidCompatible;
	}
	else
	{
		ownerVersion = g_IOSCompatible;
	}
	
	std::vector<std::string> vOwnerVersionNum;
	SplitString(ownerVersion, vOwnerVersionNum, ".");

	for (size_t i = 0; i < vOwnerVersionNum.size(); ++i)
	{
		if (vRecvVersionNum[i] < vOwnerVersionNum[i])
		{
			std::cout << "(other) 版本错误: -3" << std::endl;
			std::cout << "(other) 接收到的版本：" << vRecvVersion << std::endl;
			std::cout << "(other) 本地的版本：" << ownerVersion << std::endl;
			return -3;
		}
	}

	return 0;
}

int IsVersionCompatible( std::string recvVersion )
{
	if (recvVersion.size() == 0)
	{
		std::cout << "版本错误：-1" << std::endl;
		return -1;
	}

	std::vector<std::string> vRecvVersion;
	SplitString(recvVersion, vRecvVersion, "_");
	if (vRecvVersion.size() != 2 && vRecvVersion.size() != 3)
	{
		std::cout << "版本错误：-2" << std::endl;
		return -2;
	}

	int versionPrefix = std::stoi(vRecvVersion[0]);
	if (versionPrefix > 4 || versionPrefix < 1)
	{
		std::cout << "版本错误：-3" << std::endl;
		return -3;
	}
	
	switch(versionPrefix)
	{
		case 1:
		{
			
			if ( 0 != IsLinuxVersionCompatible(vRecvVersion) )
			{
				return -4;
			}
			break;
		}
		case 2:
		{
			
			return -5;
		}
		case 3:
		{
			
			if ( 0 != IsOtherVersionCompatible(vRecvVersion[1], false) )
			{
				return -6;
			}
			break;
		}
		case 4:
		{
			
			if ( 0 != IsOtherVersionCompatible(vRecvVersion[1], true) )
			{
				return -6;
			}
			break;
		}
		default:
		{
			return -7;
		}
	}
	return 0;
}

bool IsAllowSign(CTransaction &tx)
{
	char buf[512] = {0};
	GetDefault58Addr(buf, sizeof(buf));

	std::string default58Addr(buf, strlen(buf));

	for (int i = 0; i < tx.vout_size(); ++i)
	{
		CTxout txOut = tx.vout(i);
		if (txOut.scriptpubkey() == default58Addr)
		{
			return false;
		}
	}

	return true;
}

void HandleTx( const std::shared_ptr<TxMsg>& msg, const MsgData& msgdata)
{
	std::string tx_hash;
	DoHandleTx( msg, msgdata, tx_hash );
}



int DoHandleTx( const std::shared_ptr<TxMsg>& msg, const MsgData& msgdata, std::string & tx_hash )
{
	TxMsgAck txMsgAck;
	txMsgAck.set_version(getVersion());
	
	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		error("HandleTx IsVersionCompatible");
		return -1;
	}
	if(!checkTop(msg->top()))
	{
		return -2;
	}
	std::string RecvTxData = msg->tx();

	bool enable = false;
	Singleton<Config>::get_instance()->GetEnable(kConfigEnabelTypeTx, &enable);
	if (! enable)
	{
		txMsgAck.set_code(-1);
		txMsgAck.set_message("Disable Tx");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx Disable Tx");
		return -3;
	}

	debug("Recv TX ...");

	
	CTransaction tx;
	std::vector<std::string> signedIds;  
	int verify_num;
	bool ret = txstr_parse((char *)RecvTxData.data(), (size_t)RecvTxData.size(), tx, signedIds, &verify_num);
	if(!ret)
	{
		txMsgAck.set_code(-2);
		txMsgAck.set_message("txstr_parse() error");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx txstr_parse() error");
		return -4;	
	}

	if (verify_num < g_MinNeedVerifyPreHashCount)
	{
		txMsgAck.set_code(-6);
		txMsgAck.set_message("verify_num error");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx verify_num error");
		return -19;
	}

	CalcTransactionHash(tx);

	if (! checkTransaction(tx))
	{
		txMsgAck.set_code(-3);
		txMsgAck.set_message("tx invalid");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx checkTransaction");
		return -5;
	}

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		txMsgAck.set_code(-4);
		txMsgAck.set_message("TransactionInit failed !");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx (NetMessageTx) TransactionInit failed !");
		return -6;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	int verifyPreHashCount = 0;
	std::string txBlockHash;
	std::string blockPrevHash;
	
	
	bool rc = VerifyTransactionSign(tx, verifyPreHashCount, txBlockHash, msg->txencodehash());
	
	if ( verifyPreHashCount < verify_num && ContainSelfSign(tx))
	{
		txMsgAck.set_code(-7);
		txMsgAck.set_message("Give up Sign and broadcasting because you have signed ...");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx Give up Sign and broadcasting because you have signed ... ");
		return -7;
	}
	if(!rc)
	{
		txMsgAck.set_code(-8);
		txMsgAck.set_message("TX verify sign failed");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleTx VerifyTransactionSign ");
		return -8;
	}

	
	bool isPledgeTx = false;
	nlohmann::json extra = nlohmann::json::parse(tx.extra());
	std::string txType = extra["TransactionType"].get<std::string>();
	if (txType == TXTYPE_PLEDGE)
	{
		isPledgeTx = true;
	}

	
	bool isInitAccountTx = false;
	for (int i = 0; i < tx.vout_size(); ++i)
	{
		CTxout txout = tx.vout(i);
		if (txout.scriptpubkey() == g_InitAccount)
		{
			isInitAccountTx = true;
		}
	}

	
	
	if (!isPledgeTx && !isInitAccountTx && (verifyPreHashCount != 0 && verifyPreHashCount != verify_num) )
	{
		size_t len = 512;
		char buf[len] = {0};
		GetDefault58Addr(buf, len);
		std::string defauleAddr(buf, strlen(buf));

		uint64_t amount = 0;
		SearchPledge(defauleAddr, amount);

		if (amount < g_TxNeedPledgeAmt && defauleAddr != g_InitAccount)
		{
			txMsgAck.set_code(-5);
			txMsgAck.set_message("defaule account not pledge enough assets!");
			net_send_message<TxMsgAck>(msgdata, txMsgAck);
			
			std::cout << "account: " << defauleAddr << " defaule not pledge enough assets!" << std::endl;
			std::cout << "pledge amount : " << amount << std::endl;
			error("defaule account: %s not pledge enough assets!", defauleAddr.c_str());
			return -9;
		}
	}

	
	char buf[512] = {0};
	GetDefault58Addr(buf, sizeof(buf));
	
	std::string default58Addr(buf, sizeof(buf));
	for(int i = 0; i < tx.vout_size(); i++)
	{
		CTxout txout = tx.vout(i);
		if(default58Addr == txout.scriptpubkey() && default58Addr != TxHelper::GetTxOwner(tx)[0])
		{
			error("Transaction account cannot be signed!");
			return -10;
		}
	}

	if ( verifyPreHashCount < verify_num)
	{
		tx_hash = tx.hash();
		
		std::string strSignature;
		std::string strPub;
		GetSignString(txBlockHash, strSignature, strPub);

		char buf[512] = {0};
		GetDefault58Addr(buf, sizeof(buf));
		debug("[%s] add Sign ...", buf);

		CSignPreHash * signPreHash = tx.add_signprehash();
		signPreHash->set_sign(strSignature);
		signPreHash->set_pub(strPub);
		verifyPreHashCount++;
	}

	
	txMsgAck.set_txhash(tx.hash());
	
	uint64_t mineSignatureFee = 0;
	pRocksDb->GetDeviceSignatureFee( mineSignatureFee );
	
	if(mineSignatureFee <= 0)
	{
		txMsgAck.set_code(-9);
		txMsgAck.set_message("mineSignatureFee get failed or not set!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		std::cout << "mineSignatureFee get failed or not set!" << std::endl;
		error("HandleTx mineSignatureFee get failed or not set!");
		return -11;
	}

	nlohmann::json txExtra = nlohmann::json::parse(tx.extra());
	int txOwnerPayGasFee = txExtra["SignFee"].get<int>();

	if((uint64_t)txOwnerPayGasFee < g_minSignFee || (uint64_t)txOwnerPayGasFee > g_maxSignFee)
	{
		txMsgAck.set_code(-10);
		txMsgAck.set_message("GasFee error!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		std::cout << "txOwnerPayGasFee is 0!" << std::endl;
		error("HandleTx txOwnerPayGasFee is 0!");
		return -12;
	}

	std::string ownID = net_get_self_node_id();

	if (ownID != tx.ip())
	{
		
		if(verifyPreHashCount != 0 && ((uint64_t)txOwnerPayGasFee) < mineSignatureFee )
		{
			error("HandleTx txOwnerPayGasFee < mineSignatureFee!");
			return -13;
		}

		
		rc = IsAllowSign(tx);
		if (!rc)
		{
			std::cout << "default account is tx account!" << std::endl;
			error("default account is tx account!");
			return -14;
		}
	}
 
	std::string serTx = tx.SerializeAsString();

	TxMsg sendTxMsg;
	sendTxMsg.set_top(msg->top());	
	if (verifyPreHashCount < verify_num)
	{
		std::vector<std::string> v;

		int nodeSize = verify_num * 3;
		if(verifyPreHashCount > 1)   
		{
			nodeSize = 1;
		}

		std::string nextNode;
		int ret = FindSignNode(tx, nodeSize, v);
		if( ret < 0 )
		{
			txMsgAck.set_code(-11);
			txMsgAck.set_message("FindSignNode failed!");
			net_send_message<TxMsgAck>(msgdata, txMsgAck);			
			error("HandleTx FindSignNode failed! ");
			return -15;
		}

		for(auto signedId : signedIds)
		{
			auto iter = std::find(v.begin(), v.end(), signedId);
			if (iter != v.end())
			{
				v.erase(iter);
			}
		}

		std::set<int> rSet;
		srand(time(NULL));
		int num = std::min((int)v.size(), nodeSize);
		for(int i = 0; i < num; i++)
		{
			int j = rand() % v.size();
			rSet.insert(j);		
		}

		std::vector<std::string> sendid;
		for (auto i : rSet)
		{
			sendid.push_back(v[i]);
		}
#ifndef _CA_FILTER_FUN_
		std::string myid = net_get_self_node_id();
		signedIds.push_back(myid);
		cstring *txstr = txstr_append_signid(serTx.c_str(), serTx.size(), verify_num, signedIds );
		std::string txstrtmp(txstr->str, txstr->len);

		sendTxMsg.set_version(getVersion());
		sendTxMsg.set_tx(txstrtmp);
		sendTxMsg.set_txencodehash( msg->txencodehash() );

		for(int signCoutnt = 0; signCoutnt < msg->signnodemsg_size(); signCoutnt++)
		{
			SignNodeMsg signNodeMsg = msg->signnodemsg(signCoutnt);
			std::string signPubKey = signNodeMsg.signpubkey();
			std::string signGasFee = signNodeMsg.gasfee();
			std::string signaturemsg  = signNodeMsg.signmsg();
			double signonlinetime = signNodeMsg.onlinetime();

			SignNodeMsg * psignNodeMsg = sendTxMsg.add_signnodemsg();
			psignNodeMsg->set_signpubkey( signPubKey );
			psignNodeMsg->set_gasfee( signGasFee );
			psignNodeMsg->set_onlinetime(signonlinetime);
			psignNodeMsg->set_signmsg(signaturemsg);
		}

		std::string ownPubKey;
		GetPublicKey(g_publicKey, ownPubKey);

		char SignerCstr[2048] = {0};
		size_t SignerCstrLen = sizeof(SignerCstr);
		GetBase58Addr(SignerCstr, &SignerCstrLen, 0x00, ownPubKey.c_str(),ownPubKey.size());

		double mineronlinetime = 0;
		if ( 0 != pRocksDb->GetDeviceOnLineTime(mineronlinetime) )
		{
			
			
			
			
			
		}

		if(mineronlinetime == 0)
		{
			mineronlinetime = 0.00001157;
		}

		SignNodeMsg * psignNodeMsg = sendTxMsg.add_signnodemsg();
		psignNodeMsg->set_signpubkey( SignerCstr );
		psignNodeMsg->set_gasfee( std::to_string( mineSignatureFee ) );
		psignNodeMsg->set_onlinetime(mineronlinetime);
		std::string ser = psignNodeMsg->SerializeAsString();
	    std::string signatureMsg;
		std::string strPub;
		GetSignString(ser, signatureMsg, strPub);
		psignNodeMsg->set_signmsg(signatureMsg);

		cstr_free(txstr, true);
		txstr = nullptr;									
		if (sendid.size() > 0)
		{
			for (auto id : sendid)
			{
				net_send_message<TxMsg>(id.c_str(), sendTxMsg);
			}
		}
		else
		{
			error("sendid.size() <= 0");
		}
		
#endif
		txMsgAck.set_code(0);
		txMsgAck.set_message("TX need to net_broadcast");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		info("TX begin broadcast==========");
	}
	else
	{
		std::string ip = net_get_self_node_id();

		if (ip != tx.ip())
		{
			cstring *txstr = txstr_append_signid(serTx.c_str(), serTx.size(), verify_num );
			std::string txstrtmp(txstr->str, txstr->len);

			sendTxMsg.set_version(getVersion());
			sendTxMsg.set_tx(txstrtmp);
			sendTxMsg.set_txencodehash( msg->txencodehash() );

			for(int signCount = 0; signCount < msg->signnodemsg_size(); signCount++)
			{
				SignNodeMsg signNodeMsg = msg->signnodemsg( signCount );
				SignNodeMsg * psignNodeMsg = sendTxMsg.add_signnodemsg();

				psignNodeMsg->set_signpubkey(signNodeMsg.signpubkey());
				psignNodeMsg->set_gasfee(signNodeMsg.gasfee());
				psignNodeMsg->set_onlinetime(signNodeMsg.onlinetime());
				psignNodeMsg->set_signmsg(signNodeMsg.signmsg());
			}

			std::string ownPubKey;
			GetPublicKey(g_publicKey, ownPubKey);
			uint64_t mineSignatureFee = 0;
			pRocksDb->GetDeviceSignatureFee( mineSignatureFee );
			double mineronlinetime = 0;
			if ( 0 != pRocksDb->GetDeviceOnLineTime(mineronlinetime) )
			{
				
				
				
				
				
			}

			if(mineronlinetime == 0)
			{
				mineronlinetime = 0.00001157;
			}
			
			char SignerCstr[2048] = {0};
			size_t SignerCstrLen = sizeof(SignerCstr);
			GetBase58Addr(SignerCstr, &SignerCstrLen, 0x00, ownPubKey.c_str(),ownPubKey.size());
			SignNodeMsg * psignNodeMsg = sendTxMsg.add_signnodemsg();
			psignNodeMsg->set_signpubkey( SignerCstr );
			psignNodeMsg->set_gasfee( std::to_string( mineSignatureFee ) );
			psignNodeMsg->set_onlinetime( mineronlinetime );
			std::string sequencestr = psignNodeMsg->SerializeAsString();
	   		std::string signatureMsg;
			std::string strPub;
			GetSignString(sequencestr, signatureMsg, strPub);
			psignNodeMsg->set_signmsg(signatureMsg);

			cstr_free(txstr, true);
			txstr = nullptr;
#ifndef _CA_FILTER_FUN_
			
			debug("Send to ip[%s] to CreateBlock ...", tx.ip().c_str());
			net_send_message<TxMsg>(tx.ip(), sendTxMsg);
#endif
			txMsgAck.set_code(2);
			txMsgAck.set_message("TX send to Create Block");
			net_send_message<TxMsgAck>(msgdata, txMsgAck);
		}
		else
		{
			bool find;
			{
				std::lock_guard<std::mutex> lck(g_mu_tx);
				find = g_blockCacheList.find(tx.hash()) != g_blockCacheList.end();
				if(!find)
				{
					g_blockCacheList.insert(tx.hash());
				}
			}
			if(find)
			{
				txMsgAck.set_code(1);
				txMsgAck.set_message("TX has been dealed");
				net_send_message<TxMsgAck>(msgdata, txMsgAck);
				
				return -16;
			}
			
			cJSON * root = cJSON_Parse( tx.extra().data() );
			cJSON * needVerifyPreHashCountTmp = cJSON_GetObjectItem(root, "NeedVerifyPreHashCount");
			int NeedVerifyPreHashCount = needVerifyPreHashCountTmp->valueint;
			cJSON_Delete(root);

			if(verify_num != NeedVerifyPreHashCount)
			{
				txMsgAck.set_code(-14);
				txMsgAck.set_message("verify_num != NeedVerifyPreHashCount");
				net_send_message<TxMsgAck>(msgdata, txMsgAck);
				error("HandleTx verify_num != NeedVerifyPreHashCount");
				return -17;
			}	

			if( 0 != BuildBlock( serTx, msg) )
			{
				txMsgAck.set_code(-15);
				txMsgAck.set_message("BuildBlock failed");
				net_send_message<TxMsgAck>(msgdata, txMsgAck);
				error("HandleTx BuildBlock fail");
				return -18;
			}
		}
	}
	return 0;
	
}

void HandlePreTxRaw( const std::shared_ptr<TxMsgReq>& msg, const MsgData& msgdata )
{
	TxMsgAck txMsgAck;
	txMsgAck.set_version(getVersion());

	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		txMsgAck.set_code(-1);
		txMsgAck.set_message("Version incompatible!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		return ;
	}

	
	unsigned char serTxCstr[msg->sertx().size()] = {0};
	unsigned long serTxCstrLen = base64_decode((unsigned char *)msg->sertx().data(), msg->sertx().size(), serTxCstr);
	std::string serTxStr((char *)serTxCstr, serTxCstrLen);

	CTransaction tx;
	tx.ParseFromString(serTxStr);

	unsigned char strsignatureCstr[msg->strsignature().size()] = {0};
	unsigned long strsignatureCstrLen = base64_decode((unsigned char *)msg->strsignature().data(), msg->strsignature().size(), strsignatureCstr);

	unsigned char strpubCstr[msg->strsignature().size()] = {0};
	unsigned long strpubCstrLen = base64_decode((unsigned char *)msg->strpub().data(), msg->strpub().size(), strpubCstr);

	for (int i = 0; i < tx.vin_size(); i++)
	{
		CTxin * txin = tx.mutable_vin(i);
		txin->mutable_scriptsig()->set_sign( std::string( (char *)strsignatureCstr, strsignatureCstrLen ) );
		txin->mutable_scriptsig()->set_pub( std::string( (char *)strpubCstr, strpubCstrLen ) );
	}

	std::string serTx = tx.SerializeAsString();

	cJSON * root = cJSON_Parse( tx.extra().data() );
	cJSON * needVerifyPreHashCountTmp = cJSON_GetObjectItem(root, "NeedVerifyPreHashCount");
	int needVerifyPreHashCount = needVerifyPreHashCountTmp->valueint;
	cJSON_Delete(root);

	cstring *txstr = txstr_append_signid(serTx.c_str(), serTx.size(), needVerifyPreHashCount );
	std::string txstrtmp(txstr->str, txstr->len);

	TxMsg phoneToTxMsg;
	phoneToTxMsg.set_version(getVersion());
	phoneToTxMsg.set_tx(txstrtmp);
	phoneToTxMsg.set_txencodehash(msg->txencodehash());

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		std::cout << "(HandlePreTxRaw) TransactionInit failed !" << std::endl;
	}

	bool bRollback = true;
	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, bRollback);
	};

	unsigned int top = 0;
	pRocksDb->GetBlockTop(txn, top);	
	phoneToTxMsg.set_top(top);

	auto txmsg = make_shared<TxMsg>(phoneToTxMsg);
	HandleTx( txmsg, msgdata );
}

void HandleCreateTxInfoReq( const std::shared_ptr<CreateTxMsgReq>& msg, const MsgData& msgdata )
{
	CreateTxMsgAck createTxMsgAck;
	createTxMsgAck.set_version(getVersion());

	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		createTxMsgAck.set_code(-1);
		createTxMsgAck.set_message("Version incompatible!");
		net_send_message<CreateTxMsgAck>(msgdata, createTxMsgAck);
		return ;
	}

	
	std::string txData;
	int ret = CreateTransactionFromRocksDb(msg, txData);
	if( 0 != ret )
	{
		std::string sendMessage;
		int code = -2;
		if(ret == -1)
		{
			sendMessage = "parameter error!";
		}
		else
		{
			code = -3;
			sendMessage = "UTXO not found!";
		}
		
		createTxMsgAck.set_code( code );
		createTxMsgAck.set_message( sendMessage );

		net_send_message<CreateTxMsgAck>(msgdata, createTxMsgAck);
		return ;
	}

	
	size_t encodeLen = txData.size() * 2 + 1;
	unsigned char encode[encodeLen] = {0};
	memset(encode, 0, encodeLen);
	long codeLen = base64_encode((unsigned char *)txData.data(), txData.size(), encode);

	createTxMsgAck.set_code( 0 );
	createTxMsgAck.set_message( "successful!" );
	createTxMsgAck.set_txdata( (char *)encode, codeLen );

	std::string encodeStr((char *)encode, codeLen);
	std::string txEncodeHash = getsha256hash(encodeStr);
	createTxMsgAck.set_txencodehash(txEncodeHash);

	net_send_message<CreateTxMsgAck>(msgdata, createTxMsgAck);
}

void GetDeviceInfo(const std::shared_ptr<GetMacReq>& getMacReq, const MsgData& from)
{	
	std::vector<string> outmac;
	get_mac_info(outmac);
	
	std::string macstr;
	for(auto &i:outmac)
	{
		macstr += i;
	}
	string md5 = getMD5hash(macstr.c_str());
	GetMacAck getMacAck;
	getMacAck.set_mac(md5);

	net_send_message(from, getMacAck);
}

int get_mac_info(vector<string> &vec)
{	 
	int fd;
    int interfaceNum = 0;
    struct ifreq buf[16] = {0};
    struct ifconf ifc;
    char mac[16] = {0};

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        close(fd);
        return -1;
    }
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = (caddr_t)buf;
    if (!ioctl(fd, SIOCGIFCONF, (char *)&ifc))
    {
        interfaceNum = ifc.ifc_len / sizeof(struct ifreq);
        while (interfaceNum-- > 0)
        {
            
            if(string(buf[interfaceNum].ifr_name) == "lo")
            {
                continue;
            }
            if (!ioctl(fd, SIOCGIFHWADDR, (char *)(&buf[interfaceNum])))
            {
                memset(mac, 0, sizeof(mac));
                snprintf(mac, sizeof(mac), "%02x%02x%02x%02x%02x%02x",
                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[0],
                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[1],
                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[2],

                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[3],
                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[4],
                    (unsigned char)buf[interfaceNum].ifr_hwaddr.sa_data[5]);
                
                std::string s = mac;
                vec.push_back(s);
            }
            else
            {
                printf("ioctl: %s [%s:%d]\n", strerror(errno), __FILE__, __LINE__);
                close(fd);
                return -1;
            }
        }
    }
    else
    {
        printf("ioctl: %s [%s:%d]\n", strerror(errno), __FILE__, __LINE__);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int SearchPledge(const std::string &address, uint64_t &pledgeamount, std::string pledgeType)
{
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, false);
	};
	
	std::vector<string> utxos;
	int db_status = pRocksDb->GetPledgeAddressUtxo(txn, address, utxos);
	if (db_status != 0) 
	{
		return -1;
	}
	uint64_t total = 0;
	for (auto &item : utxos) 
    {
	 	std::string strTxRaw;
		if (pRocksDb->GetTransactionByHash(txn, item, strTxRaw) != 0)
		{
			continue;
		}
		CTransaction utxoTx;
		utxoTx.ParseFromString(strTxRaw);

		nlohmann::json extra = nlohmann::json::parse(utxoTx.extra());
		nlohmann::json txInfo = extra["TransactionInfo"].get<nlohmann::json>();
		std::string txPledgeType = txInfo["PledgeType"].get<std::string>();
		if (txPledgeType != pledgeType)
		{
			continue;
		}

		for (int i = 0; i < utxoTx.vout_size(); i++)
		{
			CTxout txout = utxoTx.vout(i);
			if (txout.scriptpubkey() == VIRTUAL_ACCOUNT_PLEDGE)
			{
				total += txout.value();
			}
		}
    }
	pledgeamount = total;
	return 0;
}

int FindSignNode(const CTransaction &tx, const int nextNodeNumber, std::vector<std::string> &nextNode)
{
	
	if(nextNodeNumber <= 0)
	{
		return -1;
	}

	nlohmann::json txExtra = nlohmann::json::parse(tx.extra());
	uint64_t minerFee = txExtra["SignFee"].get<int>();

	bool isPledge = false;
	std::string txType = txExtra["TransactionType"].get<std::string>();
	if (txType == TXTYPE_PLEDGE)
	{
		isPledge = true;
	}

	bool isInitAccount = false;
	std::vector<std::string> VTxowners = TxHelper::GetTxOwner(tx);
	if (VTxowners.size() == 1 && VTxowners.end() != find(VTxowners.begin(), VTxowners.end(), g_InitAccount) )
	{
		isInitAccount = true;
	}

	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if( txn == NULL )
	{
		return -2;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, false);
	};

	
	std::map<std::string, uint64_t> idsInfo = net_get_node_ids_and_fees();

	
	std::map<std::string, string> idBase58s = net_get_node_ids_and_base58address();

	std::string ownerID = net_get_self_node_id();
	
	auto iter = idsInfo.find(ownerID);
	if(iter != idsInfo.end())
	{
		idsInfo.erase(iter);
	}

	
	std::vector<std::string> txAddrs;
	for(int i = 0; i < tx.vout_size(); ++i)
	{
		CTxout txout = tx.vout(i);
		txAddrs.push_back(txout.scriptpubkey());
	}

	
	for (auto idBase58 : idBase58s)
	{
		if (txAddrs.end() != find(txAddrs.begin(), txAddrs.end(), idBase58.second))
		{
			idsInfo.erase(idBase58.first);
		}
	}

	std::vector<string> addresses;
	pRocksDb->GetPledgeAddress(txn, addresses);

	
	std::vector<std::string> pledgeNodes;

	
	for (auto idBase58 : idBase58s)
	{
		if (addresses.end() != find(addresses.begin(), addresses.end(), idBase58.second))
		{
			uint64_t amount = 0;
			if (SearchPledge(idBase58.second, amount) != 0)
			{
				continue;
			}
			if (amount >= g_TxNeedPledgeAmt)
			{
				
				pledgeNodes.push_back(idBase58.first);
			}
		}
	}

	
	std::vector< std::pair<std::string, uint64_t> > idsSortInfo;
	for(auto i : idsInfo)
	{
		
		if(i.second <= minerFee && i.second > 0)
		{
			auto iter = idBase58s.find(i.first);
			if (iter != idBase58s.end())
			{
				
				if ( (isPledge && pledgeNodes.size() < g_minPledgeNodeNum) || isInitAccount)
				{
					idsSortInfo.push_back( std::pair<std::string, uint64_t>(i.first, i.second) );
					
				}
				else
				{
					
					if( pledgeNodes.end() == find(pledgeNodes.begin(), pledgeNodes.end(), iter->first) )
					{
						continue;
					}

					
					idsSortInfo.push_back( std::pair<std::string, uint64_t>(i.first, i.second) );	
				}
			}
		}
	}

	if ( 0 != RandomSelectNode(idsSortInfo, nextNodeNumber, nextNode ) )
	{
		return -3;
	}

	return 0;
}

void GetOnLineTime()
{
	static time_t startTime = time(NULL);
	std::cout << "初始化startTime：" << startTime << std::endl;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	Transaction* txn = pRocksDb->TransactionInit();
	if(!txn) 
	{
		std::cout << " TransactionInit failed !" << std::endl;
		return ;
	}

	ON_SCOPE_EXIT{
		pRocksDb->TransactionDelete(txn, true);
	};

	{
		
		double minertime = 0.0;
		if (0 != pRocksDb->GetDeviceOnLineTime(minertime))
		{
			if ( 0 != pRocksDb->SetDeviceOnlineTime(0.00001157) )
			{
				error("(GetOnLineTime) SetDeviceOnlineTime failed!");
				return;
			}
		}

		if (minertime > 365.0)
		{
			if ( 0 != pRocksDb->SetDeviceOnlineTime(0.00001157) )
			{
				error("(GetOnLineTime) SetDeviceOnlineTime failed!");
				return;
			}
		}
	}

	
	std::vector<std::string> vTxHashs;
	std::string addr = g_AccountInfo.DefaultKeyBs58Addr;
	int db_get_status = pRocksDb->GetAllTransactionByAddreess(txn, addr, vTxHashs); 	
	if (db_get_status != 0) 
	{

	}

	std::vector<Node> vnode = net_get_public_node();
	if(vTxHashs.size() >= 1 && vnode.size() >= 1 )
	{
		double onLineTime = 0.0;
		if ( 0 != pRocksDb->GetDeviceOnLineTime(onLineTime) )
		{
			std::cout << "获取在线时长失败, SetDeviceOnlineTime" << std::endl;
			std::cout << "start: " << startTime << std::endl;
			if ( 0 != pRocksDb->SetDeviceOnlineTime(0.00001157) )
			{
				error("(GetOnLineTime) SetDeviceOnlineTime failed!");
				return;
			}
			return ;
		}

		time_t endTime = time(NULL);
		time_t dur = difftime(endTime, startTime);
		std::cout << "endTime: " << endTime << std::endl;
		std::cout << "dur: " << dur << std::endl;
		double durDay = (double)dur / (1*60*60*24);
		
		double minertime = 0.0;
		if (0 != pRocksDb->GetDeviceOnLineTime(minertime))
		{
			error("(GetOnLineTime) GetDeviceOnLineTime failed!");
			return ;
		}

		double accumatetime = durDay + minertime; 
		std::cout << "当前在线时长：" << accumatetime << " 天" << std::endl;
		if ( 0 != pRocksDb->SetDeviceOnlineTime(accumatetime) )
		{
			error("(GetOnLineTime) SetDeviceOnlineTime failed!");
			return ;
		}
		
		startTime = endTime;
		std::cout << "更新在初始时间：" << startTime << std::endl << std::endl << std::endl;
	}
	else
	{
		startTime = time(NULL);
	}
	
	if ( 0 != pRocksDb->TransactionCommit(txn) )
	{
		error("(GetOnLineTime) TransactionCommit failed!");
		return ;
	}
}

int PrintOnLineTime()
{
	double  onlinetime;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	int db_status = pRocksDb->GetDeviceOnLineTime(onlinetime);
	cout <<"onlinetiem ="<< onlinetime <<endl;
	if(db_status == 0)
	{
		cout<<"Get the data success"<<endl;
		return 0;
	}    
	return -1;        
}

int TestSetOnLineTime()
{
	std::cout <<"请输入设备的在线时长"<<std::endl;
	std::string time;
	std::cin >> time;
	auto pRocksDb = MagicSingleton<Rocksdb>::GetInstance();
	std::stringstream ssAmount(time);
	double day;
	ssAmount >> day;
  	int db_status = pRocksDb->SetDeviceOnlineTime(day);
	if(db_status == 0)
	{
		cout<<"set the data success"<<endl;
		return 0;
	}
	return -1;
}

void alarmcountfunc()
{
	if(g_ready == true)
	{
		return ;
	}
	else
	{
		g_ready = true;
		for(int i = 0; i < 7200; i++)
		{
			MinutesCountLock.lock();
			minutescount--;
			MinutesCountLock.unlock();
			sleep(1);	
		}

		g_VerifyPasswordCount = 0;
		g_ready = false;
	}
}



void HandleVerifyDevicePassword( const std::shared_ptr<VerifyDevicePasswordReq>& msg, const MsgData& msgdata )
{	
	VerifyDevicePasswordAck verifyDevicePasswordAck;
	verifyDevicePasswordAck.set_version(getVersion());

	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		verifyDevicePasswordAck.set_code(-1);
		verifyDevicePasswordAck.set_message("version error!");
		net_send_message<VerifyDevicePasswordAck>(msgdata, verifyDevicePasswordAck);
		error("HandleBuileBlockBroadcastMsg IsVersionCompatible");
		return ;
	}

	
	if(g_ready == false)
	{
		
		string  minerpasswd = Singleton<Config>::get_instance()->GetDevPassword();
		std::string passwordStr = generateDeviceHashPassword(msg->password());


		if( !passwordStr.compare( minerpasswd ) )
		{
			g_VerifyPasswordCount = 0 ;
			verifyDevicePasswordAck.set_code(0);
			verifyDevicePasswordAck.set_message("The password of miner is right");
			net_send_message<VerifyDevicePasswordAck>(msgdata, verifyDevicePasswordAck);
		}
		else 
		{
			
			g_VerifyPasswordCount++;
			verifyDevicePasswordAck.set_code(-2);
			verifyDevicePasswordAck.set_message("password error!");
			net_send_message<VerifyDevicePasswordAck>(msgdata, verifyDevicePasswordAck);
			
			
			if(g_VerifyPasswordCount >= 10 && g_ready == false)
			{
				
				std::cout << "进入倒计时！" << std::endl;
				
				std::thread counter(alarmcountfunc);
				counter.detach();
			}
		}
	}
	else
	{
		

		MinutesCountLock.lock_shared();
						
		std::cout << "倒计时剩余：" << minutescount << std::endl;

				
		std::string minutescountStr = std::to_string(minutescount);
		MinutesCountLock.unlock_shared();

		verifyDevicePasswordAck.set_code(-10);
		verifyDevicePasswordAck.set_message( minutescountStr );
		net_send_message<VerifyDevicePasswordAck>(msgdata, verifyDevicePasswordAck);
	}

	return ;
}


void HandleCreateDeviceTxMsgReq( const std::shared_ptr<CreateDeviceTxMsgReq>& msg, const MsgData& msgdata )
{
	
	TxMsgAck txMsgAck;
	txMsgAck.set_version(getVersion());

	
	if( 0 != IsVersionCompatible( msg->version() ) )
	{
		txMsgAck.set_code(-1);
		txMsgAck.set_message("version error!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);
		error("HandleCreateDeviceTxMsgReq IsVersionCompatible");
		return ;
	}

	std::string password = msg->password();
    std::string hashOriPass = generateDeviceHashPassword(password);
    std::string targetPassword = Singleton<Config>::get_instance()->GetDevPassword();
    if (hashOriPass != targetPassword) 
    {
        txMsgAck.set_code(-5);
        txMsgAck.set_message("password error!");
        net_send_message<TxMsgAck>(msgdata, txMsgAck);
        error("password error!");
        return;
    }
	
	if(msg->from().size() <= 0 || msg->to().size() <= 0 || msg->amt().size() <= 0 ||
		msg->minerfees().size() <= 0 || msg->needverifyprehashcount().size() <= 0)
	{
		txMsgAck.set_code(-2);
		txMsgAck.set_message("parameter error!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);

		error("HandleCreateDeviceTxMsgReq parameter error!");
		return ;
	}

	if( std::stod( msg->minerfees() ) < 0 || std::stoi( msg->needverifyprehashcount() ) < 3 )
	{
		txMsgAck.set_code(-3);
		txMsgAck.set_message("minerfees or needverifyprehashcount error!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);

		error("HandleCreateDeviceTxMsgReq parameter error!");
		return ;
	}

	int ret = CreateTx(msg->from().c_str(), msg->to().c_str(), msg->amt().c_str(), NULL, std::stoi(msg->needverifyprehashcount()), msg->minerfees().c_str());
	if(ret < 0)
	{
		txMsgAck.set_code(-4);
		txMsgAck.set_message("CreateTx failed!");
		net_send_message<TxMsgAck>(msgdata, txMsgAck);

		error("HandleCreateDeviceTxMsgReq CreateTx failed!!");
		return ;
	}

	txMsgAck.set_code(0);
	txMsgAck.set_message("CreateTx successful! Waiting for broadcast!");
	net_send_message<TxMsgAck>(msgdata, txMsgAck);

	debug("HandleCreateDeviceTxMsgReq CreateTx successful! Waiting for broadcast! ");
	return ;
}
