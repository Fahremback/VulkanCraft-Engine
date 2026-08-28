#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
namespace engine::jobs{enum class JobState:std::uint8_t{Queued,Running,Completed,Failed,Cancelled,TimedOut};struct JobArtifact{std::string name;std::string path;std::uint64_t bytes{0};};struct JobSnapshot{std::uint64_t id{0};JobState state{JobState::Queued};double progress{0};std::string stage;std::string error;std::vector<JobArtifact>artifacts;};class IJobService{public:virtual~IJobService()=default;virtual std::uint64_t start(const std::string&,std::uint64_t,std::string&)=0;virtual bool update(std::uint64_t,double,const std::string&,std::string&)=0;virtual bool complete(std::uint64_t,const std::vector<JobArtifact>&,std::string&)=0;virtual bool fail(std::uint64_t,const std::string&,std::string&)=0;virtual bool poll(std::uint64_t,JobSnapshot&,std::string&)const=0;virtual bool cancel(std::uint64_t,std::string&)=0;virtual bool wait(std::uint64_t,std::uint64_t,JobSnapshot&,std::string&)=0;virtual std::vector<JobSnapshot>list()const=0;};std::unique_ptr<IJobService>create_job_service();}
