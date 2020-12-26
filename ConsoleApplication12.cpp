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
}access_t;//PE���甭�s���ꂽ�A�N�Z�X�̏ڍ׏��

class event {
public:
	function<void(access_t)> callback;
	access_t access;
	int event_priority;//�L���b�V���ւ̃��N�G�X�g�����C�x���g�A�L���b�V�����̒T���C�x���g�A�L���b�V���ւ�DRAM����̏������݃C�x���g�APE�ւ̃��C�g�o�b�N�C�x���g�̏��ŏ���,���ꂼ��D��x��20,15,5,0�Ɛݒ肷��
	bool operator< (const event& second)const {
		return event_priority < second.event_priority;
	};
};//�A�N�Z�X�ɂ���Ĕ������Ă����C�x���g
const int wheelsize = 700000;//timewheel�̔z��T�C�Y
int hitcount = 0;//�L���b�V����hit����,�������͓���phase���ł��Ԃ��Ă����A�N�Z�X�̐�
int misscount = 0;//�L���b�V����miss�����A�N�Z�X�̐�
const int datasize = 4;//�L���b�V�����̃f�[�^��2^4 word����
const int associativity = 2;//�L���b�V���̘A�z�x=2^2
const int indexsize = 1;//�L���b�V���̃C���f�b�N�X�̐�=2^9
const int cunum = 1;//�L���b�V���������邩
const int wavefrontnum = 8;
const int phasenum = 8;
const int penum = 8;
const int max_tree_depth = 384;
const int wavefrontsize = phasenum * penum;
long long now = 0;//���݂̃T�C�N��
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
	bool valid;//�L���b�V���̗L��bit
	int tag;//�^�O
	int last_access;//�ŏI�A�N�Z�X�T�C�N���@LRU�p
}cacheline;
cacheline cache[cunum][1 << indexsize][1 << associativity];//�L���b�V��
int calculation_time[wavefrontnum] = { 0 };
priority_queue<event> timewheel[wheelsize]; // 2�ȏ��event���o�^�\
											// ������ƂȂ��Ă�(�����̎���)
int time_cal(int time);
void request_cache(access_t access);
void cache_search(access_t access);
void cache_writeback(access_t access);
void pe_writeback(access_t access);
int read_input();
void memory_read();
void arithmetic_start();
void dram_return(access_t access);
int time_cal(int time) {//timewheel�̂ǂ��ɃC�x���g��o�^���邩�v�Z
	return (time) % wheelsize;
}
void request_cache(access_t access) {
	registerlist.push(access);
	if (registerlist.size() == 1) {
		timewheel[now].push(event{ cache_search, access, 15});
	}
}
void cache_search(access_t access) {//�L���b�V����hit����
	cache_work_time += 1;
	access = registerlist.front();
	registerlist.pop();
	bool hit = false;
	int cu = 0;//cu�ԍ�
	uint32_t address = access.address;//�A�N�Z�X�������A�h���X
	uint32_t tag = address >> (indexsize + datasize + 2);//�A�h���X�ɑΉ�����^�O
	uint32_t index = (address % (1 << (indexsize + datasize + 2 ))) >> (datasize + 2);//�A�h���X�ɑΉ��������C���f�b�N�X
	for (int i = 0; i < 1 << associativity; i++) {
		if (cache[cu][index][i].valid && cache[cu][index][i].tag == tag) {//valid�Ń^�O�̈�v����L���b�V�����C�����������ꍇhit�B�ŐV�A�N�Z�X���Ԃ��X�V
			hit = true;
			cache[cu][index][i].last_access = now;
			break;
		}
	}
	if (hit) {
		timewheel[time_cal(now + 3)].push(event{ pe_writeback, access, 0 });//3�N���b�N��ɑΉ�����wavefront,phase�̃X�g�[��������
	}
	else {
		if (dramlist.count(address) == 0) {
			dramlist.insert(address);
			max_dram_wait = max(max_dram_wait, (int)dramlist.size());
			int oldest = -1;//�X�V����L���b�V�����C���̔ԍ��B�ȉ��̃��[�v�őS�Ẵ��C�����C�e���[�V�������čX�V
			int latest_access = INT_MAX;//�X�V����L���b�V�����C���̍ŏI�A�N�Z�X����
										//�X�V����L���b�V�����C���̌���BLRU�ōs���B
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
	int cu = 0;//cu�ԍ�
	uint32_t address = access.address;//�A�N�Z�X�������A�h���X
	uint32_t tag = address >> (indexsize + datasize + 2);//�A�h���X�ɑΉ�����^�O
	uint32_t index = (address % (1 << (indexsize + datasize + 2))) >> (datasize + 2);//�A�h���X�ɑΉ��������C���f�b�N�X
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
//�A�N�Z�X���N�G�X�g�@�A�N�Z�X�I���@�Z�p���Z�@�A�N�Z�X���N�G�X�g
//�K�v�ȃC�x���g
// �L���b�V���ւ�PE����̃��N�G�X�g�̔��s
// �L���b�V�������΂炭�r�W�[�ɂ���
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
	cout << "���݂̃t���[��: " << frame_count+1<<"\n";
	cout << "��������: " << now << "\n";
	cout << "PE�ғ���: " << (double)(now-pe_idle_time) / now << "\n";
	cout << "�L���b�V���ғ���: " << (double)cache_work_time / now << "\n";
	cout << "Wavefront���Ƃ̏�������\n";
	for (int wa = 0; wa < wavefrontnum; wa++) {
		cout << calculation_time[wa] << " ";
	}
	cout << "\n";
	cout << "�ő��DRAM�҂���: " << max_dram_wait << "\n";
	cout << "�L���b�V���̒ǂ��o����: " << num_cache_out << "\n";
	cout << "\n";
}
int main(int argc, char* argv[]) {
	std::ifstream in("sample_kd_tree_converted.txt");
	std::cin.rdbuf(in.rdbuf());
	ofstream ofs("simulation_result.txt");
	cout.rdbuf(ofs.rdbuf());
	clear_values();
	while (read_input() == 0) {//1�t���[�����̓��͂�ǂݍ���ŁAwhile���[�v�̒��̏����ɓ���
		while (1) {
			if (now==991) {
				int x;
				x = 1;
			}
			if (registerlist.size() > 0) {
				timewheel[now].push(event{ cache_search, NULL, 15 });
			}
			while (!timewheel[time_cal(now)].empty()) {
				//�L���b�V���̃C�x���g������
				event currentevent = timewheel[now].top();
				timewheel[now].pop();
				access_t access = currentevent.access;
				auto func = currentevent.callback;
				func(access);
			}
			if (arithmetic_count > 0) {//�Z�p���Z���̏ꍇ
				arithmetic_count--;//�Z�p���Z�̎c�莞�Ԃ��f�N�������g
				if (arithmetic_count == 0) {//�Z�p���Z���I��������
					if (processingdepth[wavefront] == treedepth[wavefront]) {//�������݂�wavefront�ɂ��Ă��ׂĂ̒T�����I�����Ă����
						is_stall[wavefront] = true;//����wavefront���X�g�[��������
						accessedaddress[wavefront] = 0;
						for (int ph = 0; ph < phasenum; ph++) {
							for (int pe = 0; pe < penum; pe++) {
								accessed[wavefront][ph][pe] = false;
							}
						}
						calculation_time[wavefront] = now;//����wavefront�ɂ��Čv�Z���Ԃ���������
						wavefront += 1;
						bool finished = true;
						for (int wa = 0; wa < wavefrontnum; wa++) {//�ق���wavefront���܂ߌ��݂̃t���[���̌v�Z�����ׂďI�����Ă��Ȃ����m�F����
							if (calculation_time[wa] == 0) {
								finished = false;
							}
						}
						if (finished) {//�������݂̃t���[���̂��ׂĂ̌v�Z���I�����Ă�����A���̃t���[���֍s��
							output_result();
							frame_count += 1;
							break;
						}
					}
					else {//���݂�wavefront�̏������܂��c���Ă�����
						processingdepth[wavefront] += 1;//���̐[���Ɉڂ�
						accessedaddress[wavefront] = 0;//�������A�N�Z�X���I������PE�̐���0�ɂ���
						for (int ph = 0; ph < phasenum; ph++) {
							for (int pe = 0; pe < penum; pe++) {
								accessed[wavefront][ph][pe] = false;
							}
						}
						is_stall[wavefront] = false;//�X�g�[���͂��Ă��Ȃ���Ԃɂ���
					}
				}
			}
			else {
				bool memoryread_flag = false;//Load���Z���s���t���O
				bool arithmetic_flag = false;//�Z�p���Z���s���t���O
				if (phase == 0) {
					for (int add = 0; add < wavefrontnum; add++) {//���݂�wavefront���珇�ɁA���Z�����s�\���m�F
						int currentwavefront = (wavefront + add) % wavefrontnum;
						if (is_stall[currentwavefront] == false) {//�����X�g�[����ԂłȂ���΁ALoad���Z���s��
							wavefront = currentwavefront;
							memoryread_flag = true;
							break;
						}
						else {
							if (accessedaddress[currentwavefront] == wavefrontsize) {//�X�g�[�����Ă���A�����ׂẴ������ǂݍ��݂��I�����Ă���ΎZ�p���Z���s��
								wavefront = currentwavefront;
								arithmetic_flag = true;
								break;
							}
						}
					}
				}
				else {
					memoryread_flag = true;//phase 0�łȂ���ΕK��Load���Z�̓r��
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
