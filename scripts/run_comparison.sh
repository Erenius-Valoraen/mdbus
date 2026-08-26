sudo ./scripts/benchmode.sh off &&
./build/pingpong_legacy before_optimisation.csv && python plots/plot_run.py  before_optimisation.csv -o before_optimisation.png &&
sudo ./scripts/benchmode.sh on &&
./build/pingpong_legacy after_optimisation.csv && python plots/plot_run.py after_optimisation.csv -o after_optimisation.png