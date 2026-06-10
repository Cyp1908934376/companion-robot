# CompanionRobot iOS build helpers
#
# Prerequisites:
#   brew install xcodegen    (project generation)
#   Xcode 15+ (build)

.PHONY: project open clean build

# Generate .xcodeproj from project.yml
project:
	xcodegen generate

# Open in Xcode
open:
	open CompanionRobot.xcodeproj

# Clean build artifacts
clean:
	rm -rf ~/Library/Developer/Xcode/DerivedData/CompanionRobot-*

# Build for simulator
build:
	xcodebuild -project CompanionRobot.xcodeproj \
		-scheme CompanionRobot \
		-destination 'platform=iOS Simulator,name=iPhone 15' \
		build
