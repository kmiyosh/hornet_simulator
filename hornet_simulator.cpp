// 10/19 version
#include <iostream>
#include <utility>
#include <string>
#include <algorithm>
#include <map>
#include <vector>
#include <climits>
#include <numeric>
#include <stdio.h>
#include <queue>
#include <iomanip>
#include <float.h>
#include <set>
#include <functional>
#include <stack>
#include <time.h>
#include <climits>
#include <bitset>
#include <fstream>
#include <stdlib.h>
#include <stdint.h>
using namespace std;


typedef struct access_t {
	uint32_t address;
	int wavefront;
	int phase;
	int line;
}access_t;//PEから発行されたアクセスの詳細情報

class event {
public:
	function<void(access_t)> callback;
	access_t access;
	int event_priority;//キャッシュへのリクエスト到着イベント、キャッシュ内の探索イベント、キャッシュへのDRAMからの書き込みイベント、PEへのライトバックイベントの順で処理,それぞれ優先度を20,15,5,0と設定する
	bool operator< (const event& second)const {
		return event_priority < second.event_priority;
	};
};//アクセスによって発生していくイベント
const int wheelsize = 700000;//timewheelの配列サイズ
int hitcount = 0;//キャッシュにhitした,もしくは同一phase内でかぶっていたアクセスの数
int misscount = 0;//キャッシュでmissしたアクセスの数
const int datasize = 4;//キャッシュ内のデータは2^4 wordある
const int associativity = 2;//キャッシュの連想度=2^2
const int indexsize = 1;//キャッシュのインデックスの数=2^9
const int cunum = 1;//キャッシュが何個あるか
const int wavefrontnum = 8;
const int phasenum = 8;
const int penum = 8;
const int max_tree_depth = 384;
const int wavefrontsize = phasenum * penum;
long long now = 0;//現在のサイクル
uint32_t pattern[wavefrontnum][max_tree_depth][phasenum][penum];
int treedepth[wavefrontnum] = { 0 };
int processingdepth[wavefrontnum] = { 0 };
bool is_stall[wavefrontnum];
int accessedaddress[wavefrontnum];
int frame_count = 0;
set<int>addresslist;
int wavefront = 0;
int phase = 0;
int arithmetic_count = 0;
int max_dram_wait = 0;
int num_cache_out = 0;
int pe_idle_time = 0;
int cache_work_time = 0;
queue<access_t>registerlist;
set<uint32_t>dramlist;
bool accessed[wavefrontnum][phasenum][penum];
typedef struct cacheline {
	bool valid;//キャッシュの有効bit
	int tag;//タグ
	int last_access;//最終アクセスサイクル　LRU用
}cacheline;
cacheline cache[cunum][1 << indexsize][1 << associativity];//キャッシュ
int calculation_time[wavefrontnum] = { 0 };
priority_queue<event> timewheel[wheelsize]; // 2つ以上のeventも登録可能
											// くるっとなってる(無限の実装)
int time_cal(int time);
void request_cache(access_t access);
void cache_search(access_t access);
void cache_writeback(access_t access);
void pe_writeback(access_t access);
int read_input();
void memory_read();
void arithmetic_start();
void dram_return(access_t access);
int time_cal(int time) {//timewheelのどこにイベントを登録するか計算
	return (time) % wheelsize;
}
void request_cache(access_t access) {
	registerlist.push(access);
	if (registerlist.size() == 1) {
		timewheel[now].push(event{ cache_search, access, 15});
	}
}
void cache_search(access_t access) {//キャッシュのhit判定
	cache_work_time += 1;
	access = registerlist.front();
	registerlist.pop();
	bool hit = false;
	int cu = 0;//cu番号
	uint32_t address = access.address;//アクセスしたいアドレス
	uint32_t tag = address >> (indexsize + datasize + 2);//アドレスに対応するタグ
	uint32_t index = (address % (1 << (indexsize + datasize + 2 ))) >> (datasize + 2);//アドレスに対応したいインデックス
	for (int i = 0; i < 1 << associativity; i++) {
		if (cache[cu][index][i].valid && cache[cu][index][i].tag == tag) {//validでタグの一致するキャッシュラインがあった場合hit。最新アクセス時間を更新
			hit = true;
			cache[cu][index][i].last_access = now;
			break;
		}
	}
	if (hit) {
		timewheel[time_cal(now + 3)].push(event{ pe_writeback, access, 0 });//3クロック後に対応するwavefront,phaseのストールを解除
	}
	else {
		if (dramlist.count(address) == 0) {
			dramlist.insert(address);
			max_dram_wait = max(max_dram_wait, (int)dramlist.size());
			int oldest = -1;//更新するキャッシュラインの番号。以下のループで全てのラインをイテレーションして更新
			int latest_access = INT_MAX;//更新するキャッシュラインの最終アクセス時間
										//更新するキャッシュラインの決定。LRUで行う。
			for (int i = 0; i < 1 << associativity; i++) {
				if (!cache[cu][index][i].valid) {
					oldest = i;
					latest_access = -1;
				}
				else {
					if (cache[cu][index][i].last_access < latest_access) {
						oldest = i;
						latest_access = cache[cu][index][i].last_access;
					}
				}
			}
			if (cache[cu][index][oldest].valid) { num_cache_out += 1; }
			cache[cu][index][oldest].valid = false;
			timewheel[time_cal(now + 205)].push(event{ cache_writeback, access, 5});
			timewheel[time_cal(now + 205)].push(event{ dram_return, access, 0});
		}
	}
}
void cache_writeback(access_t access) {
	int cu = 0;//cu番号
	uint32_t address = access.address;//アクセスしたいアドレス
	uint32_t tag = address >> (indexsize + datasize + 2);//アドレスに対応するタグ
	uint32_t index = (address % (1 << (indexsize + datasize + 2))) >> (datasize + 2);//アドレスに対応したいインデックス
	int rewriteline = access.line;
	cache[cu][index][rewriteline].valid = true;
	cache[cu][index][rewriteline].tag = tag;
	cache[cu][index][rewriteline].last_access = now;
}
void pe_writeback(access_t access) {
	int wavefront = access.wavefront;
	int phase = access.phase;
	for (int pe = 0; pe < penum; pe++) {
		if (pattern[wavefront][processingdepth[wavefront]][phase][pe] == access.address) {
			accessed[wavefront][phase][pe] = true;
			accessedaddress[wavefront] += 1;
		}
	}
}
void dram_return(access_t access) {
	uint32_t address = access.address;
	dramlist.erase(address);
	for (int wa = 0; wa < wavefrontnum; wa++) {
		for (int ph = 0; ph < phasenum; ph++) {
			for (int pe = 0; pe < penum; pe++) {
				if (accessed[wa][ph][pe]==false&&pattern[wa][processingdepth[wa]][ph][pe] == access.address) {
					accessed[wa][ph][pe] = true;
					accessedaddress[wa] += 1;
				}
			}
		}
	}
}
void clear_values() {
	now = 0;
	hitcount = 0;
	num_cache_out = 0;
	max_dram_wait=0;
	pe_idle_time = 0;
	cache_work_time = 0;
	for (int wa = 0; wa < wavefrontnum; wa++) {
		treedepth[wa] = 0;
		processingdepth[wa] = 0;
		calculation_time[wa] = 0;
		is_stall[wa] = false;
		for (int tr = 0; tr < max_tree_depth; tr++) {
			for (int ph = 0; ph < phasenum; ph++) {
				for (int pe = 0; pe < penum; pe++) {
					pattern[wa][tr][ph][pe] = 0;
				}
			}
		}
	}
}
int read_input() {
	int wavefront, phase, t, pe;
	while (cin >>dec>> wavefront >> t >> phase) {
		treedepth[wavefront] = max(treedepth[wavefront], t);
		if (wavefront == -1) {
			return 0;
		}
		for (int pe = 0; pe < penum; pe++) {
			cin >> hex >> pattern[wavefront][t][phase][pe];
		}
	}
	return 1;
}
//アクセスリクエスト　アクセス終了　算術演算　アクセスリクエスト
//必要なイベント
// キャッシュへのPEからのリクエストの発行
// キャッシュをしばらくビジーにする
//
void memory_read() {
	addresslist.clear();
	for (int pe = 0; pe < penum; pe++) {
		if (pattern[wavefront][processingdepth[wavefront]][phase][pe] == 0) {
			accessedaddress[wavefront] += 1;
		}
		else {
			addresslist.insert(pattern[wavefront][processingdepth[wavefront]][phase][pe]);
		}
	}
	for (auto itr = addresslist.begin(); itr != addresslist.end(); itr++) {
		access_t access = { *itr, wavefront, phase };
		timewheel[time_cal(now + 2)].push(event{ request_cache, access, 0 });
	}
	phase += 1;
	if (phase >= phasenum) {
		is_stall[wavefront] = true;
		phase = 0;
		wavefront += 1;
		if (wavefront >= wavefrontnum) {
			wavefront = 0;
		}
	}
}
void arithmetic_start() {
	arithmetic_count = 56;
}
void output_result() {
	cout << "現在のフレーム: " << frame_count+1<<"\n";
	cout << "処理時間: " << now << "\n";
	cout << "PE稼動率: " << (double)(now-pe_idle_time) / now << "\n";
	cout << "キャッシュ稼動率: " << (double)cache_work_time / now << "\n";
	cout << "Wavefrontごとの処理時間\n";
	for (int wa = 0; wa < wavefrontnum; wa++) {
		cout << calculation_time[wa] << " ";
	}
	cout << "\n";
	cout << "最大のDRAM待ち数: " << max_dram_wait << "\n";
	cout << "キャッシュの追い出し数: " << num_cache_out << "\n";
	cout << "\n";
}
int main(int argc, char* argv[]) {
	std::ifstream in("sample_kd_tree_converted.txt");
	std::cin.rdbuf(in.rdbuf());
	ofstream ofs("simulation_result.txt");
	cout.rdbuf(ofs.rdbuf());
	clear_values();
	while (read_input() == 0) {//1フレーム分の入力を読み込んで、whileループの中の処理に入る
		while (1) {
			if (now==991) {
				int x;
				x = 1;
			}
			if (registerlist.size() > 0) {
				timewheel[now].push(event{ cache_search, NULL, 15 });
			}
			while (!timewheel[time_cal(now)].empty()) {
				//キャッシュのイベントを処理
				event currentevent = timewheel[now].top();
				timewheel[now].pop();
				access_t access = currentevent.access;
				auto func = currentevent.callback;
				func(access);
			}
			if (arithmetic_count > 0) {//算術演算中の場合
				arithmetic_count--;//算術演算の残り時間をデクリメント
				if (arithmetic_count == 0) {//算術演算が終了したら
					if (processingdepth[wavefront] == treedepth[wavefront]) {//もし現在のwavefrontについてすべての探索が終了していれば
						is_stall[wavefront] = true;//そのwavefrontをストールさせる
						accessedaddress[wavefront] = 0;
						for (int ph = 0; ph < phasenum; ph++) {
							for (int pe = 0; pe < penum; pe++) {
								accessed[wavefront][ph][pe] = false;
							}
						}
						calculation_time[wavefront] = now;//そのwavefrontについて計算時間をメモする
						wavefront += 1;
						bool finished = true;
						for (int wa = 0; wa < wavefrontnum; wa++) {//ほかのwavefrontも含め現在のフレームの計算がすべて終了していないか確認する
							if (calculation_time[wa] == 0) {
								finished = false;
							}
						}
						if (finished) {//もし現在のフレームのすべての計算が終了していたら、次のフレームへ行く
							output_result();
							frame_count += 1;
							break;
						}
					}
					else {//現在のwavefrontの処理がまだ残っていたら
						processingdepth[wavefront] += 1;//次の深さに移る
						accessedaddress[wavefront] = 0;//メモリアクセスが終了したPEの数を0にする
						for (int ph = 0; ph < phasenum; ph++) {
							for (int pe = 0; pe < penum; pe++) {
								accessed[wavefront][ph][pe] = false;
							}
						}
						is_stall[wavefront] = false;//ストールはしていない状態にする
					}
				}
			}
			else {
				bool memoryread_flag = false;//Load演算を行うフラグ
				bool arithmetic_flag = false;//算術演算を行うフラグ
				if (phase == 0) {
					for (int add = 0; add < wavefrontnum; add++) {//現在のwavefrontから順に、演算を実行可能か確認
						int currentwavefront = (wavefront + add) % wavefrontnum;
						if (is_stall[currentwavefront] == false) {//もしストール状態でなければ、Load演算を行う
							wavefront = currentwavefront;
							memoryread_flag = true;
							break;
						}
						else {
							if (accessedaddress[currentwavefront] == wavefrontsize) {//ストールしており、かつすべてのメモリ読み込みが終了していれば算術演算を行う
								wavefront = currentwavefront;
								arithmetic_flag = true;
								break;
							}
						}
					}
				}
				else {
					memoryread_flag = true;//phase 0でなければ必ずLoad演算の途中
				}
				if (memoryread_flag) {
					memory_read();
				}
				else {
					if (arithmetic_flag) {
						arithmetic_start();
					}
					else {
						pe_idle_time += 1;
					}
				}
			}
			now += 1;
		}
		clear_values();
	}
}
