#include "videoprocessor.h"
#include <string>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/video.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <coin/ClpSimplex.hpp>
#include <coin/ClpModel.hpp>
#include <coin/OsiSolverInterface.hpp>
#include <coin/OsiClpSolverInterface.hpp>
#include <QList>
#include "frame.h"
#include "video.h"
#include "displacement.h"
#include "ransacmodel.h"
#include "tools.h"
#include "graphdrawer.h"
#include "l1model.h"
#include <stdio.h>
#include <iostream>
#include <QDebug>
#include <QObject>
#include <vector>
#include <engine.h>

using namespace std;

VideoProcessor::VideoProcessor(QObject *parent):QObject(parent) {
    QObject::connect(&outlierRejector, SIGNAL(progressMade(int, int)), this, SIGNAL(progressMade(int, int)));
}

VideoProcessor::~VideoProcessor() {
}

void VideoProcessor::reset(){
    qDebug() << "Not yet implemented";
    delete video;
}

void VideoProcessor::calculateGlobalMotion() {
    detectFeatures();
    trackFeatures();
    outlierRejection();
    calculateMotionModel();
}

void VideoProcessor::loadVideo(QString path) {
    qDebug() << "Loading video";
    emit processStarted(VIDEO_LOADING);
    cv::VideoCapture vc;
    bool videoOpened = vc.open(path.toStdString());
    if (!videoOpened) {
        qDebug() << "VideoProcessor::loadVideo - Video could not be opened";
        emit processFinished(VIDEO_LOADING);
        return;
    }
    videoPath = path;
    Mat buffer;
    Mat bAndWBuffer;
    int numFrames = vc.get(CV_CAP_PROP_FRAME_COUNT);
    int fps = vc.get(CV_CAP_PROP_FPS);
    video = new Video(numFrames,fps);
    int currentFrame = 0;
    while (vc.read(buffer)) {
        emit progressMade(currentFrame, numFrames-1);
        //cvtColor(buffer, bAndWBuffer, CV_BGR2GRAY);
        Frame* newFrame = new Frame(buffer.clone(),video);
        video->appendFrame(newFrame);
        currentFrame++;
    }
    emit videoLoaded(video);
    emit processFinished(VIDEO_LOADING);
    return;
}

void VideoProcessor::detectFeatures() {
    emit processStarted(FEATURE_DETECTION);
    qDebug() << "VideoProcessor::detectFeatures - Feature Detection started";
    int frameCount = video->getFrameCount();
    FeatureDetector* featureDetector = new GoodFeaturesToTrackDetector();
    vector<KeyPoint> bufferPoints;
    for (int i = 0; i < frameCount; i++) {
        qDebug() << "VideoProcessor::detectFeatures - Detecting features in frame " << i <<"/"<<frameCount-1;
        emit progressMade(i, frameCount-1);
        Frame* frame = video->accessFrameAt(i);
        featureDetector->detect(frame->getOriginalData(), bufferPoints);
        vector<Point2f> features;
        KeyPoint::convert(bufferPoints, features);
        frame->setFeatures(features);
        qDebug() << "VideoProcessor::detectFeatures - Detected " << bufferPoints.size() << " features";
    }
    delete featureDetector;
    emit videoUpdated(video);
    emit processFinished(FEATURE_DETECTION);
    return;
}

void VideoProcessor::trackFeatures() {
    emit processStarted(FEATURE_TRACKING);
    qDebug() << "VideoProcessor::trackFeatures - Feature Tracking started";
    for (int i = video->getFrameCount()-1; i > 0; i--) {
        Frame* frameT = video->accessFrameAt(i);
        const Frame* framePrev = video->accessFrameAt(i-1);
        const vector<Point2f>& features = frameT->getFeatures();
        int featuresToTrack = frameT->getFeatures().size();
        qDebug() << "VideoProcessor::trackFeatures - Tracking " << featuresToTrack << " features from frame " << i << " to frame "<<i-1;
        emit progressMade(video->getFrameCount()-i, video->getFrameCount());
        vector<Point2f> nextPositions;
        vector<uchar> status;
        vector<float> err;
        // Initiate optical flow tracking
        calcOpticalFlowPyrLK(frameT->getOriginalData(),
                             framePrev->getOriginalData(),
                             features,
                             nextPositions,
                             status,
                             err);
        // Remove features that were not tracked correctly
        int featuresCorrectlyTracked = 0;
        for (uint j = 0; j < features.size(); j++) {
            if (status[j] == 0) {
                // Feature could not be tracked
            } else {
                // Feature was tracked
                featuresCorrectlyTracked++;
                Displacement d = Displacement(features[j], nextPositions[j]);
                frameT->registerDisplacement(d);
            }
        }
        qDebug() << "VideoProcessor::trackFeatures - " << featuresCorrectlyTracked << "/" << featuresToTrack << "successfully tracked";
    }
    emit videoUpdated(video);
    emit processFinished(FEATURE_TRACKING);
    return;
}

void VideoProcessor::outlierRejection() {
    emit processStarted(OUTLIER_REJECTION);
    qDebug() << "VideoProcessor::outlierRejection - Outlier rejection started";
    outlierRejector.execute(video);
    qDebug() << "VideoProcessor::outlierRejection - Outlier rejection finished";
    emit videoUpdated(video);
    emit processFinished(OUTLIER_REJECTION);
    return;
}

void VideoProcessor::calculateMotionModel() {
    emit processStarted(ORIGINAL_MOTION);
    qDebug() << "VideoProcessor::calculateMotionModel - Calculating original motion";
    for (int i = 1; i < video->getFrameCount(); i++) {
        emit progressMade(i, video->getFrameCount()-2);
        Frame* frame = video->accessFrameAt(i);
        vector<Point2f> srcPoints, destPoints;
        frame->getInliers(srcPoints,destPoints);
        Mat affineTransform = estimateRigidTransform(srcPoints, destPoints, true);
//        std::stringstream ss;
//        ss << "F(t="<<i<<") = " << affineTransform;
//        qDebug() << QString::fromStdString(ss.str());
        frame->setAffineTransform(affineTransform);
    }
    qDebug() << "VideoProcessor::calculateMotionModel - Original motion detected";
    emit processFinished(ORIGINAL_MOTION);
}

void VideoProcessor::calculateUpdateTransform() {
    emit processStarted(STILL_MOTION);
    qDebug() << "VideoProcessor::calculateUpdateTransform - Start";
    // Build model
    L1Model model(video);
    emit progressMade(1,3);
    qDebug() << "VideoProcessor::calculateUpdateTransform - Solving L1 Problem";
    // Solve model
    model.solve();
    emit progressMade(2,3);
    // Extract Results
    for (int t = 1; t < video->getFrameCount(); t++)
    {
        Mat m(2,3,DataType<float>::type);
        for (char letter = 'a'; letter <= 'e'; letter++) {
            m.at<float>(L1Model::toRow(letter),L1Model::toCol(letter)) = model.getVariableSolution(t, letter);
        }
//        std::stringstream ss;
//        ss << "B(t="<<t<<") = " << m;
//        qDebug() << QString::fromStdString(ss.str());
        Frame* f = video->accessFrameAt(t);
        f->setUpdateTransform(m);
    }
    emit progressMade(3,3);
    qDebug() << "VideoProcessor::calculateUpdateTransform - Ideal Path Calculated";
    emit processFinished(STILL_MOTION);
}

void VideoProcessor::applyCropTransform()
{
    qDebug() << "VideoProcessor::applyCropTransform() - Started";
    emit processStarted(CROP_TRANSFORM);
    croppedVideo = new Video(video->getFrameCount());
    Rect cropWindow = video->getCropBox();
    qDebug() << cropWindow.x << "," << cropWindow.y << " width:" << cropWindow.width << " height: " << cropWindow.height;
    for (int f = 0; f < video->getFrameCount(); f++) {
        emit progressMade(f, video->getFrameCount());
        const Frame* frame = video->getFrameAt(f);
        const Mat& img = frame->getOriginalData();
        //Move cropWindow from current position to next position using frame's update transform
        //Extract rectangle
        cv::Mat croppedImage = img(cropWindow);
        Frame* croppedF = new Frame(croppedImage, croppedVideo);
        croppedVideo->appendFrame(croppedF);
    }
    emit processFinished(CROP_TRANSFORM);
    qDebug() << "VideoProcessor::applyCropTransform() - Finished";
}

void VideoProcessor::saveCroppedVideo(QString path)
{
    qDebug() << "VideoProcessor::saveCroppedVideo() - Started";
    saveVideo(croppedVideo, path);
    qDebug() << "VideoProcessor::saveCroppedVideo() - Finished";
}

void VideoProcessor::saveVideo(const Video* videoToSave, QString path)
{
    qDebug() << "VideoProcessor::saveVideo() - Started";
    emit processStarted(SAVING_VIDEO);
    String fp = path.toStdString();
    Size frameSize = videoToSave->getSize();
    VideoWriter record(fp, CV_FOURCC('I','Y','U','V'),video->getOrigFps(), videoToSave->getSize());
    assert(record.isOpened());
    for (int f = 0; f < videoToSave->getFrameCount(); f++) {
        emit progressMade(f, videoToSave->getFrameCount());
        const Frame* frame = videoToSave->getFrameAt(f);
        const Mat& img = frame->getOriginalData();
        record << img;
    }
    emit processFinished(SAVING_VIDEO);
    qDebug() << "VideoProcessor::saveVideo() - Finished";
}



