// Copyright (c) 2018 Baidu.com, Inc. All Rights Reserved
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gflags/gflags.h>
#include <bthread/bthread.h>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <braft/raft.h>
#include <braft/util.h>
#include <braft/route_table.h>
#include "counter.pb.h"
#include <vector>

DEFINE_bool(log_each_request, false, "Print log for each request");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_int32(add_percentage, 100, "Percentage of fetch_add");
DEFINE_int64(added_by, 1, "Num added to each peer");
DEFINE_int32(thread_num, 1, "Number of threads sending requests");
DEFINE_int32(timeout_ms, 10000, "Timeout for each request");
DEFINE_string(conf, "", "Configuration of the raft group");
DEFINE_string(group, "Counter", "Id of the replication group");
DEFINE_bool(response_redundancy, false, "Send get request to all replicas and wait for their responses");

bvar::LatencyRecorder g_latency_recorder("counter_client");

static void* sender(void* arg) {
    while (!brpc::IsAskedToQuit()) {
        braft::PeerId leader;
        // Select leader of the target group from RouteTable
        if (braft::rtb::select_leader(FLAGS_group, &leader) != 0) {
            // Leader is unknown in RouteTable. Ask RouteTable to refresh leader
            // by sending RPCs.
            butil::Status st = braft::rtb::refresh_leader(
                        FLAGS_group, FLAGS_timeout_ms);
            if (!st.ok()) {
                // Not sure about the leader, sleep for a while and the ask again.
                LOG(WARNING) << "Fail to refresh_leader : " << st;
                bthread_usleep(FLAGS_timeout_ms * 1000L);
            }
            continue;
        }

        // Now we known who is the leader, construct Stub and then sending
        // rpc
        brpc::Channel channel;
        if (channel.Init(leader.addr, NULL) != 0) {
            LOG(ERROR) << "Fail to init channel to " << leader;
            bthread_usleep(FLAGS_timeout_ms * 1000L);
            continue;
        }
        example::CounterService_Stub stub(&channel);

        brpc::Controller cntl;
        cntl.set_timeout_ms(FLAGS_timeout_ms);
        // Randomly select which request we want send;
        example::CounterResponse response;

        if (butil::fast_rand_less_than(100) < (size_t)FLAGS_add_percentage) {
            example::FetchAddRequest request;
            request.set_value(FLAGS_added_by);
            stub.fetch_add(&cntl, &request, &response, NULL);

            if (cntl.Failed()) {
                LOG(WARNING) << "Fail to send request to " << leader
                            << " : " << cntl.ErrorText();
                // Clear leadership since this RPC failed.
                braft::rtb::update_leader(FLAGS_group, braft::PeerId());
                bthread_usleep(FLAGS_timeout_ms * 1000L);
                continue;
            }
            if (!response.success()) {
                LOG(WARNING) << "Fail to send request to " << leader
                            << ", redirecting to "
                            << (response.has_redirect() 
                                    ? response.redirect() : "nowhere");
                // Update route table since we have redirect information
                braft::rtb::update_leader(FLAGS_group, response.redirect());
                continue;
            }
            g_latency_recorder << cntl.latency_us();
            if (FLAGS_log_each_request) {
                LOG(INFO) << "[WRITE] Change value: " 
                        <<  response.value()
                        << " to " << response.value()+1 ;
                bthread_usleep(1000L * 1000L);
            }
        } 
        else {

            example::GetRequest request;

            if (FLAGS_response_redundancy) { //send request to all replicas
                std::vector<braft::PeerId> peers;
                std::vector<example::CounterResponse> responses; 

                for (butil::StringSplitter sp(FLAGS_conf.c_str(), ','); sp; ++sp) {
                    std::string peer_str(sp.field(), sp.length());  
                    braft::PeerId peer;
                    if (peer.parse(peer_str) != 0) {
                        LOG(WARNING) << "Invalid peer format: " << peer_str;
                        continue;
                    }
                    peers.push_back(peer);
                }

                if (peers.empty()) {
                    LOG(WARNING) << "No valid peers found in configuration: " << FLAGS_conf;
                    bthread_usleep(FLAGS_timeout_ms * 1000L);
                    continue;
                }

                for (const auto& peer : peers) {
                    brpc::Channel peer_channel;
                    if (peer_channel.Init(peer.addr, NULL) != 0) {
                        LOG(ERROR) << "Fail to init channel to " << peer;
                        continue;
                    }
                    
                    example::CounterService_Stub peer_stub(&peer_channel);
                    brpc::Controller peer_cntl;
                    peer_cntl.set_timeout_ms(FLAGS_timeout_ms);
                    example::CounterResponse peer_response;

                    example::GetRequest request;
                    peer_stub.get(&peer_cntl, &request, &peer_response, NULL);

                    if (peer_cntl.Failed()) {
                        LOG(WARNING) << "Fail to send get request to " << peer
                                    << " : " << peer_cntl.ErrorText();
                        braft::rtb::update_leader(FLAGS_group, braft::PeerId());
                        bthread_usleep(FLAGS_timeout_ms * 1000L);
                        continue;
                    }
                    responses.push_back(peer_response);
                }
                
                if (FLAGS_log_each_request) {
                    std::unordered_map<int64_t, int> value_count;
                    
                    for (size_t i = 0; i < responses.size(); ++i) {
                        int64_t value = responses[i].value();
                        value_count[value]++;

                        LOG(INFO) << "[READ] Response server" << i
                                << ": value=" << value;
                    }

                    int64_t majority_value = -1;
                    int max_count = 0;
                    for (const auto& pair : value_count) {
                        if (pair.second > max_count) {
                            max_count = pair.second;
                            majority_value = pair.first;
                        }
                    }

                    LOG(INFO) << "[READ DECISION] Majority Value: " << majority_value
                            << " (Appeared " << max_count << " times)";

                    bthread_usleep(1000L * 1000L);
                }
            }
            else {
                stub.get(&cntl, &request, &response, NULL);

                if (cntl.Failed()) {
                    LOG(WARNING) << "Fail to send request to " << leader
                                << " : " << cntl.ErrorText();
                    // Clear leadership since this RPC failed.
                    braft::rtb::update_leader(FLAGS_group, braft::PeerId());
                    bthread_usleep(FLAGS_timeout_ms * 1000L);
                    continue;
                }
                if (!response.success()) {
                    LOG(WARNING) << "Fail to send request to " << leader
                                << ", redirecting to "
                                << (response.has_redirect() 
                                        ? response.redirect() : "nowhere");
                    // Update route table since we have redirect information
                    braft::rtb::update_leader(FLAGS_group, response.redirect());
                    continue;
                }
                g_latency_recorder << cntl.latency_us();
                if (FLAGS_log_each_request) {
                    LOG(INFO) << "[READ] Received response from " << leader
                            << " value=" << response.value()
                            << " latency=" << cntl.latency_us();
                    bthread_usleep(1000L * 1000L);
                }
            }
            
        }
        
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    butil::AtExitManager exit_manager;

    // Register configuration of target group to RouteTable
    if (braft::rtb::update_configuration(FLAGS_group, FLAGS_conf) != 0) {
        LOG(ERROR) << "Fail to register configuration " << FLAGS_conf
                   << " of group " << FLAGS_group;
        return -1;
    }

    std::vector<bthread_t> tids;
    std::vector<pthread_t> pids;
    if (!FLAGS_use_bthread) {
        pids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&pids[i], NULL, sender, NULL) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        tids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(&tids[i], NULL, sender, NULL) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        LOG_IF(INFO, !FLAGS_log_each_request)
                << "Sending Request to " << FLAGS_group
                << " (" << FLAGS_conf << ')'
                << " at qps=" << g_latency_recorder.qps(1)
                << " latency=" << g_latency_recorder.latency(1);
    }

    LOG(INFO) << "Counter client is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(pids[i], NULL);
        } else {
            bthread_join(tids[i], NULL);
        }
    }

    return 0;
}
