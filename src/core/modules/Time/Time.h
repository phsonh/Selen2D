#pragma once

namespace core::modules::Time {
struct Date { int year,month,day,hour,minute,second; };
Date GetDate();
double FramesToSeconds(double frames,double fps);
double SecondsToFrames(double seconds,double fps);
}
