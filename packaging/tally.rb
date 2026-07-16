class Tally < Formula
  desc "GNU wc reimplementation: byte-identical output, SIMD word counting"
  homepage "https://github.com/tenseleyFlow/tally"
  url "https://github.com/tenseleyFlow/tally/releases/download/v0.2.0/tally-0.2.0.tar.gz"
  sha256 "75ee496b0b822bd9c9d20caf045df1fe2a8e00fd38f4a66d67e155f2d246a7ae"
  license "GPL-3.0-or-later"
  head "https://github.com/tenseleyFlow/tally.git", branch: "trunk"

  def install
    system "./configure"
    system "make", "release"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_match "tally #{version}", shell_output("#{bin}/tally --version")
    assert_equal %w[0 0 0], shell_output("printf '' | #{bin}/tally").split
    # ty is the same binary under a shorter name.
    assert_equal "3", shell_output("printf 'a b c' | #{bin}/ty -w").strip
  end
end
